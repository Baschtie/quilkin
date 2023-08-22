#pragma once

#include "CoreMinimal.h"
#include "QuilkinCircularBuffer.h"
#include "../Public/QuilkinEndpoint.h"
#include "IPAddress.h"

class QUILKIN_API FQuilkinInternetAddrs : public FInternetAddr {
public:
	//~ Start FInternetAddr overrides
	virtual void SetIp(uint32 InAddr) override;
	virtual void SetIp(const TCHAR* InAddr, bool& bIsValid) override;

	//~ End FInternetAddr overrides

	TOptional<EndpointPair> GetLowestLatencyEndpoint();

private:
	TSConcurrentMap<FQuilkinEndpoint, CircularBuffer<int64>> Endpoints;
};
