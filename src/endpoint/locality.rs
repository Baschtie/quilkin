/*
 * Copyright 2022 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

use std::collections::BTreeSet;

use itertools::Itertools;
use serde::{Deserialize, Serialize};

use super::Endpoint;
use crate::xds::config::endpoint::v3::LocalityLbEndpoints;

/// The location of an [`Endpoint`].
#[derive(
    Clone,
    Default,
    Debug,
    Hash,
    Eq,
    PartialEq,
    Deserialize,
    Serialize,
    schemars::JsonSchema,
    PartialOrd,
    Ord,
)]
pub struct Locality {
    /// The geographic region.
    #[serde(default)]
    pub region: String,
    /// The zone within the `region`, if applicable.
    #[serde(default)]
    pub zone: String,
    /// The subzone within the `zone`, if applicable.
    #[serde(default)]
    pub sub_zone: String,
}

impl Locality {
    pub fn new(
        region: impl Into<String>,
        zone: impl Into<String>,
        sub_zone: impl Into<String>,
    ) -> Self {
        Self {
            region: region.into(),
            zone: zone.into(),
            sub_zone: sub_zone.into(),
        }
    }

    pub fn region(region: impl Into<String>) -> Self {
        Self {
            region: region.into(),
            ..Self::default()
        }
    }

    pub fn zone(mut self, zone: impl Into<String>) -> Self {
        self.zone = zone.into();
        self
    }

    pub fn colon_separated_string(&self) -> String {
        let mut string = String::from(&*self.region);

        if !self.zone.is_empty() {
            string += ":";
            string += &*self.zone;
        }

        if !self.sub_zone.is_empty() {
            string += ":";
            string += &*self.sub_zone;
        }

        string
    }

    pub fn sub_zone(mut self, sub_zone: impl Into<String>) -> Self {
        self.sub_zone = sub_zone.into();
        self
    }
}

/// A set of endpoints optionally grouped by a [`Locality`].
#[derive(
    Clone,
    Default,
    Debug,
    Eq,
    PartialEq,
    PartialOrd,
    Ord,
    Deserialize,
    Serialize,
    schemars::JsonSchema,
)]
pub struct LocalityEndpoints {
    pub locality: Option<Locality>,
    pub endpoints: BTreeSet<Endpoint>,
}

impl LocalityEndpoints {
    /// Creates a new set of endpoints with no [`Locality`].
    pub fn new(endpoints: BTreeSet<Endpoint>) -> Self {
        Self::from(endpoints)
    }

    /// Adds a [`Locality`] to the set of endpoints.
    pub fn with_locality(mut self, locality: impl Into<Option<Locality>>) -> Self {
        self.locality = locality.into();
        self
    }

    /// Removes an endpoint.
    pub fn remove(&mut self, endpoint: &Endpoint) {
        self.endpoints.remove(endpoint);
    }
}

impl From<Endpoint> for LocalityEndpoints {
    fn from(endpoint: Endpoint) -> Self {
        Self {
            endpoints: [endpoint].into_iter().collect(),
            ..Self::default()
        }
    }
}

impl From<Vec<Endpoint>> for LocalityEndpoints {
    fn from(endpoints: Vec<Endpoint>) -> Self {
        Self {
            endpoints: endpoints.into_iter().collect(),
            ..Self::default()
        }
    }
}

impl From<Vec<std::net::SocketAddr>> for LocalityEndpoints {
    fn from(endpoints: Vec<std::net::SocketAddr>) -> Self {
        Self {
            endpoints: endpoints.into_iter().map(From::from).collect(),
            ..Self::default()
        }
    }
}

impl From<BTreeSet<Endpoint>> for LocalityEndpoints {
    fn from(endpoints: BTreeSet<Endpoint>) -> Self {
        Self {
            endpoints,
            ..Self::default()
        }
    }
}

impl From<crate::xds::config::core::v3::Locality> for Locality {
    fn from(value: crate::xds::config::core::v3::Locality) -> Self {
        Self {
            region: value.region,
            zone: value.zone,
            sub_zone: value.sub_zone,
        }
    }
}

impl From<Locality> for crate::xds::config::core::v3::Locality {
    fn from(value: Locality) -> Self {
        Self {
            region: value.region,
            zone: value.zone,
            sub_zone: value.sub_zone,
        }
    }
}

impl TryFrom<LocalityLbEndpoints> for LocalityEndpoints {
    type Error = <Endpoint as TryFrom<crate::xds::config::endpoint::v3::LbEndpoint>>::Error;
    fn try_from(value: LocalityLbEndpoints) -> Result<Self, Self::Error> {
        Ok(Self {
            endpoints: value
                .lb_endpoints
                .into_iter()
                .map(TryFrom::try_from)
                .collect::<Result<_, Self::Error>>()?,
            locality: value.locality.map(From::from),
        })
    }
}

impl From<(Vec<Endpoint>, Option<Locality>)> for LocalityEndpoints {
    fn from((endpoints, locality): (Vec<Endpoint>, Option<Locality>)) -> Self {
        Self::from(endpoints).with_locality(locality)
    }
}

impl From<(Endpoint, Locality)> for LocalityEndpoints {
    fn from((endpoint, locality): (Endpoint, Locality)) -> Self {
        Self::from(endpoint).with_locality(locality)
    }
}

impl From<(Endpoint, Option<Locality>)> for LocalityEndpoints {
    fn from((endpoint, locality): (Endpoint, Option<Locality>)) -> Self {
        Self::from(endpoint).with_locality(locality)
    }
}

impl From<LocalityEndpoints> for LocalityLbEndpoints {
    fn from(value: LocalityEndpoints) -> Self {
        Self {
            lb_endpoints: value.endpoints.into_iter().map(From::from).collect(),
            locality: value.locality.map(From::from),
            ..Self::default()
        }
    }
}

/// Set around [`LocalityEndpoints`] to ensure that all unique localities are
/// different entries. Any duplicate localities provided are merged.
#[derive(Clone, Default, Debug, Eq, PartialEq)]
pub struct LocalitySet(std::collections::HashMap<Option<Locality>, LocalityEndpoints>);

impl LocalitySet {
    /// Creates a new set from the provided localities.
    pub fn new(set: Vec<LocalityEndpoints>) -> Self {
        Self::from_iter(set)
    }

    /// Inserts a new locality of endpoints.
    pub fn insert(&mut self, mut locality: LocalityEndpoints) {
        let entry = self.0.entry(locality.locality.clone()).or_default();
        entry.locality = locality.locality;
        entry.endpoints.append(&mut locality.endpoints);
    }

    /// Returns a reference to endpoints associated with the specified locality.
    pub fn get(&self, key: &Option<Locality>) -> Option<&LocalityEndpoints> {
        self.0.get(key)
    }

    /// Returns a mutable reference to endpoints associated with the specified locality.
    pub fn get_mut(&mut self, key: &Option<Locality>) -> Option<&mut LocalityEndpoints> {
        self.0.get_mut(key)
    }

    /// Removes the specified locality or all endpoints with no locality.
    pub fn remove(&mut self, key: &Option<Locality>) -> Option<LocalityEndpoints> {
        self.0.remove(key)
    }

    /// Removes all localities.
    pub fn clear(&mut self) {
        self.0.clear();
    }

    /// Returns an iterator over the set of localities.
    pub fn iter(&self) -> impl Iterator<Item = &LocalityEndpoints> + '_ {
        self.0.values()
    }

    /// Returns a mutable iterator over the set of localities.
    pub fn iter_mut(&mut self) -> impl Iterator<Item = &mut LocalityEndpoints> + '_ {
        self.0.values_mut()
    }

    pub fn merge(&mut self, cluster: &Self) {
        use std::collections::hash_map::Entry;

        for (key, value) in &cluster.0 {
            let span = tracing::info_span!(
                "applied_locality",
                locality = &*key
                    .as_ref()
                    .map(|locality| locality.colon_separated_string())
                    .unwrap_or_else(|| String::from("<none>"))
            );

            let _entered = span.enter();

            match self.0.entry(key.clone()) {
                // The eviction logic is as follows:
                //
                // If an endpoint already exists:
                // - If `sessions` is zero then it is dropped.
                // If that endpoint exists in the new set:
                // - Its metadata is replaced with the new set.
                // Else the endpoint remains.
                //
                // This will mean that updated metadata such as new tokens
                // will be respected, but we will still retain older
                // endpoints that are currently actively used in a session.
                Entry::Occupied(entry) => {
                    let (key, original_locality) = entry.remove_entry();
                    let mut value = value.clone();

                    if tracing::enabled!(tracing::Level::INFO) {
                        for endpoint in value.endpoints.iter() {
                            tracing::info!(
                                %endpoint.address,
                                endpoint.tokens=%endpoint.metadata.known.tokens.iter().map(crate::utils::base64_encode).join(", "),
                                "applying endpoint"
                            );
                        }
                    }

                    let (retained, dropped): (Vec<_>, _) = original_locality
                        .endpoints
                        .into_iter()
                        .partition(|endpoint| {
                            crate::proxy::sessions::ADDRESS_MAP
                                .get(&endpoint.address)
                                .is_some()
                        });

                    if tracing::enabled!(tracing::Level::INFO) {
                        for endpoint in dropped {
                            tracing::info!(
                                %endpoint.address,
                                endpoint.tokens=%endpoint.metadata.known.tokens.iter().map(crate::utils::base64_encode).join(", "),
                                "dropping endpoint"
                            );
                        }
                    }

                    for endpoint in retained {
                        tracing::info!(
                            %endpoint.address,
                            endpoint.tokens=%endpoint.metadata.known.tokens.iter().map(crate::utils::base64_encode).join(", "),
                            "retaining endpoint"
                        );

                        value.endpoints.insert(endpoint);
                    }

                    self.0.insert(key, value.clone());
                }
                Entry::Vacant(entry) => {
                    tracing::info!("adding new locality");
                    entry.insert(value.clone());
                }
            }
        }
    }
}

impl Serialize for LocalitySet {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        self.0.values().collect::<Vec<_>>().serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for LocalitySet {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        <Vec<LocalityEndpoints>>::deserialize(deserializer).map(Self::new)
    }
}

impl schemars::JsonSchema for LocalitySet {
    fn schema_name() -> String {
        <Vec<LocalityEndpoints>>::schema_name()
    }
    fn json_schema(gen: &mut schemars::gen::SchemaGenerator) -> schemars::schema::Schema {
        <Vec<LocalityEndpoints>>::json_schema(gen)
    }

    fn is_referenceable() -> bool {
        <Vec<LocalityEndpoints>>::is_referenceable()
    }
}

impl std::ops::Index<&Option<Locality>> for LocalitySet {
    type Output = LocalityEndpoints;

    fn index(&self, index: &Option<Locality>) -> &Self::Output {
        self.get(index).unwrap()
    }
}

impl std::ops::IndexMut<&Option<Locality>> for LocalitySet {
    fn index_mut(&mut self, index: &Option<Locality>) -> &mut Self::Output {
        self.get_mut(index).unwrap()
    }
}

impl<T> From<T> for LocalitySet
where
    T: Into<Vec<LocalityEndpoints>>,
{
    fn from(value: T) -> Self {
        Self::new(value.into())
    }
}

impl FromIterator<LocalityEndpoints> for LocalitySet {
    fn from_iter<I: IntoIterator<Item = LocalityEndpoints>>(iter: I) -> Self {
        let mut map = Self(<_>::default());

        for locality in iter {
            map.insert(locality);
        }

        map
    }
}

impl IntoIterator for LocalitySet {
    type Item = LocalityEndpoints;
    type IntoIter = std::vec::IntoIter<Self::Item>;

    fn into_iter(self) -> Self::IntoIter {
        // Have to convert to vec to avoid `Values`'s lifetime parameter.
        // Remove once GAT's are stable.
        self.0.into_values().collect::<Vec<_>>().into_iter()
    }
}
