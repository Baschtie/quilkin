pub mod agones;

use std::sync::Arc;

use futures::Stream;
use k8s_openapi::api::core::v1::ConfigMap;
use kube::runtime::watcher::Event;

use agones::GameServer;

use crate::endpoint::{Endpoint, Locality, LocalityEndpoints};

pub fn update_filters_from_configmap(
    client: kube::Client,
    namespace: impl AsRef<str>,
    config: Arc<crate::Config>,
) -> impl Stream<Item = crate::Result<(), eyre::Error>> {
    async_stream::stream! {
        for await event in configmap_events(client, namespace) {
            tracing::trace!("new configmap event");

            let event = match event {
                Ok(event) => event,
                Err(error) => {
                    yield Err(error.into());
                    continue;
                }
            };

            let configmap = match event {
                Event::Applied(configmap) => configmap,
                Event::Restarted(configmaps) => match configmaps.get(0) {
                    Some(configmap) => configmap.clone(),
                    None => {
                        yield Ok(());
                        continue;
                    },
                },
                Event::Deleted(_) => {
                    config.filters.remove();
                    yield Ok(());
                    continue;
                }
            };

            let data = configmap.data.ok_or_else(|| eyre::eyre!("configmap data missing"))?;
            let data = data.get("quilkin.yaml").ok_or_else(|| eyre::eyre!("quilkin.yaml property not found"))?;
            let data: serde_json::Map<String, serde_json::Value> = serde_yaml::from_str(data)?;

            if let Some(filters) = data
                .get("filters")
                    .cloned()
                    .map(serde_json::from_value)
                    .transpose()?
            {
                config.filters.store(Arc::new(filters));
            }

            yield Ok(());
        }
    }
}

fn configmap_events(
    client: kube::Client,
    namespace: impl AsRef<str>,
) -> impl Stream<Item = Result<Event<ConfigMap>, kube::runtime::watcher::Error>> {
    let config_namespace = namespace.as_ref();
    let configmap: kube::Api<ConfigMap> = kube::Api::namespaced(client, config_namespace);
    let config_writer = kube::runtime::reflector::store::Writer::<ConfigMap>::default();
    let configmap_stream = kube::runtime::watcher(
        configmap,
        kube::runtime::watcher::Config::default().labels("quilkin.dev/configmap=true"),
    );
    kube::runtime::reflector(config_writer, configmap_stream)
}

fn gameserver_events(
    client: kube::Client,
    namespace: impl AsRef<str>,
) -> impl Stream<Item = Result<Event<GameServer>, kube::runtime::watcher::Error>> {
    let gameservers_namespace = namespace.as_ref();
    let gameservers: kube::Api<GameServer> = kube::Api::namespaced(client, gameservers_namespace);
    let gs_writer = kube::runtime::reflector::store::Writer::<GameServer>::default();
    let gameserver_stream = kube::runtime::watcher(gameservers, <_>::default());
    kube::runtime::reflector(gs_writer, gameserver_stream)
}

pub fn update_endpoints_from_gameservers(
    client: kube::Client,
    namespace: impl AsRef<str>,
    config: Arc<crate::Config>,
    locality: Option<Locality>,
) -> impl Stream<Item = crate::Result<(), eyre::Error>> {
    async_stream::stream! {
        for await event in gameserver_events(client, namespace) {
            match event? {
                Event::Applied(server) => {
                    if !server.is_allocated() {
                        yield Ok(());
                        continue;
                    }

                    let endpoint = match Endpoint::try_from(server) {
                        Ok(endpoint) => endpoint,
                        Err(error) => {
                            tracing::warn!(%error, "received invalid gameserver to apply from k8s");
                            continue;
                        }
                    };
                    tracing::info!(endpoint=%serde_json::to_value(&endpoint).unwrap(), "adding endpoint");
                    match &locality {
                        Some(locality) => config
                            .clusters
                            .write()
                            .default_cluster_mut()
                            .insert((endpoint, locality.clone())),
                        None => config
                            .clusters
                            .write()
                            .default_cluster_mut()
                            .insert(endpoint),
                    };
                    tracing::trace!(clusters=%serde_json::to_value(&config.clusters).unwrap(), "current clusters");
                }

                Event::Restarted(servers) => {
                    let servers: Vec<_> = servers
                        .into_iter()
                        .filter(GameServer::is_allocated)
                        .map(Endpoint::try_from)
                        .filter_map(|result| {
                            match result {
                                Ok(endpoint) => Some(endpoint),
                                Err(error) => {
                                    tracing::warn!(%error, "received invalid gameserver on restart from k8s");
                                    None
                                }
                            }
                        })
                        .collect();
                    let endpoints = LocalityEndpoints::from((servers, locality.clone()));
                    tracing::info!(
                        endpoints=%serde_json::to_value(&endpoints).unwrap(),
                        "Restarting with endpoints"
                    );
                    config.clusters.write().default_cluster_mut().insert(endpoints);
                }

                Event::Deleted(server) => {
                    let found = if let Some(endpoint) = server.endpoint() {
                        config.clusters.write().remove_endpoint(&endpoint)
                    } else {
                        config.clusters.write().remove_endpoint_if(|endpoint| {
                            if endpoint.metadata.unknown.get("name") == server.metadata.name.clone().map(From::from).as_ref() {
                                tracing::info!(
                                    endpoint=%serde_json::to_value(endpoint).unwrap(),
                                    "deleting gameserver"
                                );
                                true
                            } else {
                                false
                            }
                        })
                    };

                    if found.is_none() {
                        tracing::warn!(
                            endpoint=%serde_json::to_value(server.endpoint()).unwrap(),
                            name=%serde_json::to_value(server.metadata.name).unwrap(),
                            "received unknown gameserver to delete from k8s"
                        );
                    } else if let Some(endpoint) = server.endpoint() {
                        tracing::info!(
                            endpoint=%serde_json::to_value(endpoint).unwrap(),
                            "deleting gameserver"
                        );
                    }
                }
            };

            config.apply_metrics();
            yield Ok(());
        }
    }
}
