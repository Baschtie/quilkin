/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "CoreMinimal.h"
#include "CoreTypes.h"
#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "../Private/QuilkinLog.h"
#include "../Private/QuilkinConcurrentMap.h"
#include "../Private/QuilkinResult.h"
#include "IPAddress.h"

#include "QuilkinEndpoint.generated.h"

class FQuilkinSocketSubsystem;

struct ResolveError {};

/* Represents a Quilkin proxy endpoint */
USTRUCT()
struct FQuilkinEndpoint {
	GENERATED_BODY()
public:
	UPROPERTY(config, EditAnywhere, Category = Quilkin)
	FString Host;
	UPROPERTY(config, EditAnywhere, Category = Quilkin)
	uint16 Port = 0;
	UPROPERTY(config, EditAnywhere, Category = Quilkin)
	FString Region;

	/* Resolves a host and port into a `FInternetAddr`, providing a `ResolveError` if there was
	   problems resolving it.  */
	const TResult<TSharedRef<FInternetAddr>, ResolveError> ToInternetAddr(FQuilkinSocketSubsystem* SocketSubsystem) const;

	const FString ToString() const
	{
		return FString::Printf(TEXT("%s:%d"), *Host, Port);
	}

	friend int32 GetTypeHash(const FQuilkinEndpoint& Endpoint)
	{
		return HashCombine(GetTypeHash(Endpoint.Host), GetTypeHash(Endpoint.Port));
	}

	friend bool operator==(const FQuilkinEndpoint& A, const FQuilkinEndpoint& B)
	{
		return A.Host == B.Host && A.Port == B.Port;
	}
};

using EndpointPair = TTuple<FQuilkinEndpoint, int64>;
