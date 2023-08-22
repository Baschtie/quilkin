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

#include "QuilkinSocketSubsystem.h"
#include "QuilkinConstants.h"
#include "QuilkinControlMessageProtocol.h"
#include "QuilkinCircularBuffer.h"
#include "QuilkinDelegates.h"
#include "QuilkinSocket.h"
#include "QuilkinLog.h"
#include "QuilkinSettings.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "IPAddress.h"
#include "Engine/GameInstance.h"

static int64 NowSeconds() {
	return FDateTime::UtcNow().ToUnixTimestamp();
}

FQuilkinSocketSubsystem::FQuilkinSocketSubsystem(ISocketSubsystem* WrappedSocketSubsystem) 
	: PingSocket{nullptr}
	, SocketSubsystem{WrappedSocketSubsystem}
{
}

FQuilkinSocketSubsystem::~FQuilkinSocketSubsystem()
{
}

bool FQuilkinSocketSubsystem::Init(FString& Error)
{
	UE_LOG(LogQuilkin, Display, TEXT("Initialising Socket Subsystem"));
	PingSocket = CreateRawSocket(TEXT("QuilkinPingSocket"));
	FQuilkinDelegates::GetLowestLatencyEndpoint.BindRaw(this, &FQuilkinSocketSubsystem::GetLowestLatencyEndpoint);
	FQuilkinDelegates::GetLowestLatencyEndpointInRegion.BindRaw(this, &FQuilkinSocketSubsystem::GetLowestLatencyEndpointInRegion);
	return true;
}

void FQuilkinSocketSubsystem::Shutdown()
{
	if (PingSocket != nullptr)
	{
		PingSocket = nullptr;
	}

	if (UQuilkinConfigSubsystem::IsAvailable()) {
		auto Config = UQuilkinConfigSubsystem::Get();
		if (!Config->OnEndpointsChanged.IsBound()) {
			Config->OnEndpointsChanged.Remove(UpdateEndpointHandle);
		}
	}

	if (FQuilkinDelegates::GetLowestLatencyEndpoint.IsBound()) {
		FQuilkinDelegates::GetLowestLatencyEndpoint.Unbind();
	}

	if (FQuilkinDelegates::GetLowestLatencyEndpointInRegion.IsBound()) {
		FQuilkinDelegates::GetLowestLatencyEndpointInRegion.Unbind();
	}
}

bool FQuilkinSocketSubsystem::Tick(float DeltaTime) {
	// We bind here as a "late initialisation" step, as the SocketSubsystem will be initialised before GEngine.
	if (UQuilkinConfigSubsystem::IsAvailable() && !UpdateEndpointHandle.IsValid()) {
		UpdateEndpointHandle = UQuilkinConfigSubsystem::Get()->OnEndpointsChanged.AddRaw(this, &FQuilkinSocketSubsystem::UpdateEndpoints);
	}

	if (!UQuilkinConfigSubsystem::Get()->GetMeasureEndpoints()) {
		return true;
	}

	TickElapsed += DeltaTime;

	if (TickElapsed >= 60) {
		TickElapsed = 0;
		if (Endpoints.IsEmpty()) {
			UE_LOG(LogQuilkin, Display, TEXT("no endpoints to measure"));
			return true;
		}

		PingEndpoints();
	}

	return true;
}

TArray<TTuple<FQuilkinEndpoint, int64>> FQuilkinSocketSubsystem::GetEndpointMeasurements() {
	return Endpoints.MapToArray<TTuple<FQuilkinEndpoint, int64>>([](FQuilkinEndpoint Endpoint, CircularBuffer<int64> Buffer) {
		return TTuple<FQuilkinEndpoint, int64>(Endpoint, Buffer.Median());
	});
}

void FQuilkinSocketSubsystem::UpdateEndpoints(TArray<FQuilkinEndpoint> NewEndpoints) {
	Endpoints.ResetWithKeys(NewEndpoints, [](auto Endpoint) -> CircularBuffer<int64> {
		return CircularBuffer<int64>(50);
	});

	if (UQuilkinConfigSubsystem::Get()->GetMeasureEndpoints()) {
		PingEndpoints();
	}
}

TOptional<EndpointPair> FQuilkinSocketSubsystem::GetLowestLatencyEndpoint()
{
	return GetLowestLatencyEndpointImplementation(TOptional<FString>());
}

TOptional<TTuple<FQuilkinEndpoint, int64>> FQuilkinSocketSubsystem::GetLowestLatencyEndpointInRegion(FString Region) const
{
	return GetLowestLatencyEndpointImplementation(TOptional<FString>(Region));
}

TOptional<TTuple<FQuilkinEndpoint, int64>> FQuilkinSocketSubsystem::GetLowestLatencyEndpointImplementation(TOptional<FString> Region) const
{
	if (Endpoints.IsEmpty()) {
		return TOptional<TTuple<FQuilkinEndpoint, int64>>();
	}

	bool NoMeasurements = true;
	Endpoints.ForEach([&NoMeasurements](FQuilkinEndpoint Endpoint, CircularBuffer<int64> Buffer) {
		if (!Buffer.IsEmpty()) {
			NoMeasurements = false;
		}
	});

	if (NoMeasurements) {
		return TOptional<TTuple<FQuilkinEndpoint, int64>>();
	}

	FQuilkinEndpoint LowestEndpoint;
	int64 LowestLatency = INT64_MAX;
	Endpoints.ForEach([&LowestEndpoint, &LowestLatency, &Region](FQuilkinEndpoint Endpoint, CircularBuffer<int64> Buffer) {
		if (Region.IsSet() && Region.GetValue() != Endpoint.Region)
		{
			return;
		}

		int64 Median = Buffer.Median();

		if (Median < LowestLatency)
		{
			LowestEndpoint = Endpoint;
			LowestLatency = Median;
		}
	});

	return TOptional(TTuple<FQuilkinEndpoint, int64>(LowestEndpoint, LowestLatency));

}

TResult<FSocket*, FString> FQuilkinSocketSubsystem::CreateRandomUdpSocket()
{
    FSocket* Socket = CreateRawSocket(TEXT("QuilkinPingSocket"));

    if (!Socket)
    {
		return TResult<FSocket*, FString>(TEXT("couldn't create ping socket"));
    }

    return TResult<FSocket*, FString>(Socket);
}

void FQuilkinSocketSubsystem::PingEndpoints() {
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]() {
		FQuilkinSocketSubsystem* Subsystem = static_cast<FQuilkinSocketSubsystem*>(ISocketSubsystem::Get(QUILKIN_SOCKETSUBSYSTEM_NAME));
		auto Keys = Endpoints.GetKeys();

		ParallelFor(Keys.Num(), [&](int32 Index) {
			auto Endpoint = Keys[Index];
			auto SocketResult = CreateRandomUdpSocket();

			if (SocketResult.IsError()) {
				UE_LOG(LogQuilkin, Warning, TEXT("%s"), *SocketResult.GetError());
				return;
			}

			auto Socket = SocketResult.GetValue();
			auto EndpointResult = Endpoint.ToInternetAddr(Subsystem);

			if (EndpointResult.IsError()) {
				UE_LOG(LogQuilkin, Warning, TEXT("Couldn't resolve %s to an IP address, adding %dms penalty"), *Endpoint.Host, NanosToMillis(DefaultPenaltyLatency));
				Endpoints.FindOrDefaultToAdd(Endpoint, DefaultPenaltyLatency);
				return;
			}
			TSharedRef<FInternetAddr> Addr = EndpointResult.GetValue();
			FInternetAddr& Ptr = Addr.Get();
			auto PingResult = SendPing(Socket, Ptr);
			if (PingResult.IsError()) {
				UE_LOG(LogQuilkin, Warning, TEXT("ping for %s failed: %s, adding %dms penalty"), *Endpoint.Host, *PingResult.GetError(), NanosToMillis(DefaultPenaltyLatency));
				Endpoints.FindOrDefaultToAdd(Endpoint, DefaultPenaltyLatency);
				return;
			}

			auto Latency = PingResult.GetValue();
			Endpoints.FindOrDefaultToAdd(Endpoint, Latency);
			});


		if (UE_LOG_ACTIVE(LogQuilkin, Display)) {
			auto Result = this->GetLowestLatencyEndpoint();

			if (!Result.IsSet()) {
				return;
			}

			auto Pair = Result.GetValue();
			FQuilkinEndpoint Endpoint = Pair.template Get<0>();
			UE_LOG(LogQuilkin, Display, TEXT("Lowest latency endpoint is %s (%dms)"), *Endpoint.ToString(), NanosToMillis(Pair.template Get<1>()));
		}
	});
}

TResult<int64, FString> FQuilkinSocketSubsystem::SendPing(FSocket* Socket, FInternetAddr& AddrRef)
{
	uint32 PingCount = 5;
	TArray<uint8> Nonces;
	auto Addr = AddrRef.Clone();
	Addr->SetPort(7600);

	for (uint32 i = 0; i < PingCount; i++) {
		auto Result = SendPacket(Socket, *Addr);
		if (Result.IsError()) {
			UE_LOG(LogQuilkin, Warning, TEXT("failed to send ping to %s: %s"), *Addr->ToString(true), *Result.GetError());
		}
		else {
			Nonces.Push(Result.GetValue());
		}
	}

	if (Nonces.IsEmpty()) {
		return TResult<int64, FString>(TEXT("all pings to failed"));
	}

	auto NewLatency = WaitForResponses(Socket, *Addr, PingCount, Nonces);
	UE_LOG(LogQuilkin, Display, TEXT("new measured latency for %s: %dms"), *Addr->ToString(true), NanosToMillis(NewLatency));
	return TResult<int64, FString>(NewLatency);
}

TResult<uint8, FString> FQuilkinSocketSubsystem::SendPacket(FSocket* Socket, FInternetAddr& Addr)
{
	auto Ping = FPing();
	auto Buffer = Ping.Encode();
	auto BytesSent = 0;
	UE_LOG(LogQuilkin, Display, TEXT("sending ping to %s, Nonce: %d"), *Addr.ToString(true), Ping.GetNonce());
	if (!Socket->SendTo(Buffer.GetData(), Buffer.Num(), BytesSent, Addr)) {
		auto ErrorCode = GetLastErrorCode();
		FString ErrorDescription = GetSocketError(ErrorCode);
		return TResult<uint8, FString>(ErrorDescription);
	}
	else {
		return TResult<uint8, FString>(Ping.GetNonce());
	}
}

int64 FQuilkinSocketSubsystem::WaitForResponses(FSocket* Socket, FInternetAddr& Addr, uint32 PingCount, TArray<uint8> Nonces)
{
	const double Timeout = 5.0;
	const double StartTime = NowSeconds();
	uint32 ExpectedResponses = Nonces.Num();
	uint32 ExceededTimeouts = 0;
	CircularBuffer<int64> SuccessfulResponses = CircularBuffer<int64>(PingCount);

	while ((SuccessfulResponses.Num() < Nonces.Num()) && (NowSeconds() - StartTime < Timeout))
	{
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(1024);
		int32 BytesReceived = 0;
		UE_LOG(LogQuilkin, Verbose, TEXT("waiting on ping response from %s"), *Addr.ToString(true));
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(500)))
		{
			if (Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), BytesReceived, Addr))
			{
				UE_LOG(LogQuilkin, Display, TEXT("received response from %s"), *Addr.ToString(true));
				auto Result = FPingReply::Decode(Buffer);

				if (Result.IsError()) {
					UE_LOG(LogQuilkin, Warning, TEXT("failed to decode ping reply: %s"), *Result.GetError());
					continue;
				}

				auto Packet = Result.GetValue()->AsVariant<FPingReply>();

				if (!Packet.IsSet()) {
					UE_LOG(LogQuilkin, Warning, TEXT("expected ping reply, found: %d"), Result.GetValue()->GetCode());
					continue;
				}

				auto Reply = Packet.GetValue();

				if (!Nonces.Contains(Reply->GetNonce())) {
					UE_LOG(LogQuilkin, Warning, TEXT("received nonce (%d) didn't match any sent nonce"), Reply->GetNonce());
					continue;
				}
				else {
					UE_LOG(LogQuilkin, Display, TEXT("received nonce (%d)"), Reply->GetNonce());
				}

				SuccessfulResponses.Add(Reply->RoundTimeDelay());
			}
		}
		else
		{
			UE_LOG(LogQuilkin, Warning, TEXT("exceeded WaitForRead timeout for %s"), *Addr.ToString(true));
			ExceededTimeouts += 1;
			continue;
		}
	}

	// If we sent less pings or we received less pings due to system errors or packet loss,
	//  then we penalise the endpoint's latency measurement for being inconsistent.
	uint32 PenaltyFactor = (PingCount - ExpectedResponses) + (ExpectedResponses - SuccessfulResponses.Num()) + ExceededTimeouts;
	for (uint32 i = 0; i < PenaltyFactor; i++) {
		SuccessfulResponses.Add(DefaultPenaltyLatency);
	}

	return SuccessfulResponses.Median();
}

// MARK: ISocketSubsystem Interface

FSocket* FQuilkinSocketSubsystem::CreateSocket(const FName& SocketType, const FString& SocketDescription, bool bForceUDP) 
{ 
	return SocketSubsystem->CreateSocket(SocketType, SocketDescription, bForceUDP); 
}

FSocket* FQuilkinSocketSubsystem::CreateSocket(const FName& SocketType, const FString& SocketDescription, const FName& ProtocolName)
{
	FSocket* WrappedSocket = SocketSubsystem->CreateSocket(SocketType, SocketDescription, ProtocolName);
	if (WrappedSocket == nullptr)
	{
		UE_LOG(LogQuilkin, Warning, TEXT("CreateSocket returned nullptr"));
		return nullptr;
	}

	ESocketType InSocketType = WrappedSocket->GetSocketType();
	auto Socket = new FQuilkinSocket(FUniqueSocket(WrappedSocket), InSocketType, SocketDescription, ProtocolName);

	Socket->Subsystem = AsWeak();
	return Socket;
}

FSocket* FQuilkinSocketSubsystem::CreateRawSocket(const FString& SocketDescription) {
	FName SocketType = NAME_DGram;
	FSocket* WrappedSocket = SocketSubsystem->CreateSocket(SocketType, SocketDescription, true);
	if (WrappedSocket == nullptr)
	{
		UE_LOG(LogQuilkin, Warning, TEXT("CreateSocket returned nullptr"));
		return nullptr;
	}

	ESocketType InSocketType = WrappedSocket->GetSocketType();
	return WrappedSocket;
};

void FQuilkinSocketSubsystem::DestroySocket(FSocket* Socket)
{
	SocketSubsystem->DestroySocket(Socket);
}

FResolveInfoCached* FQuilkinSocketSubsystem::CreateResolveInfoCached(TSharedPtr<FInternetAddr> Addr) const 
{
	return SocketSubsystem->CreateResolveInfoCached(Addr);
}

FAddressInfoResult FQuilkinSocketSubsystem::GetAddressInfo(const TCHAR* HostName, const TCHAR* ServiceName, EAddressInfoFlags QueryFlags, const FName ProtocolTypeName, ESocketType SocketType)
{
	return SocketSubsystem->GetAddressInfo(HostName, ServiceName, QueryFlags, ProtocolTypeName, SocketType);
}

void FQuilkinSocketSubsystem::GetAddressInfoAsync(FAsyncGetAddressInfoCallback Callback, const TCHAR* HostName, const TCHAR* ServiceName, EAddressInfoFlags QueryFlags, const FName ProtocolTypeName, ESocketType SocketType) 
{
	SocketSubsystem->GetAddressInfoAsync(Callback, HostName, ServiceName, QueryFlags, ProtocolTypeName, SocketType);
}

TSharedPtr<FInternetAddr> FQuilkinSocketSubsystem::GetAddressFromString(const FString& InAddress)
{
	return SocketSubsystem->GetAddressFromString(InAddress);
}

FResolveInfo* FQuilkinSocketSubsystem::GetHostByName(const ANSICHAR* HostName)
{
	return SocketSubsystem->GetHostByName(HostName);
}

bool FQuilkinSocketSubsystem::RequiresChatDataBeSeparate()
{
	return SocketSubsystem->RequiresChatDataBeSeparate();
}

bool FQuilkinSocketSubsystem::RequiresEncryptedPackets()
{
	return SocketSubsystem->RequiresEncryptedPackets();
}

bool FQuilkinSocketSubsystem::GetHostName(FString& HostName)
{
	return SocketSubsystem->GetHostName(HostName);
}

TSharedRef<FInternetAddr> FQuilkinSocketSubsystem::CreateInternetAddr()
{
	return SocketSubsystem->CreateInternetAddr();
}

TSharedRef<FInternetAddr> FQuilkinSocketSubsystem::CreateInternetAddr(const FName ProtocolType)
{
	return SocketSubsystem->CreateInternetAddr(ProtocolType);
}

TSharedRef<FInternetAddr> FQuilkinSocketSubsystem::GetLocalBindAddr(FOutputDevice& Out)
{
	return SocketSubsystem->GetLocalBindAddr(Out);
}

TArray<TSharedRef<FInternetAddr>> FQuilkinSocketSubsystem::GetLocalBindAddresses()
{
	return SocketSubsystem->GetLocalBindAddresses();
}

bool FQuilkinSocketSubsystem::GetLocalAdapterAddresses(TArray<TSharedPtr<FInternetAddr>>& OutAddresses)
{
	return SocketSubsystem->GetLocalAdapterAddresses(OutAddresses);
}

TUniquePtr<FRecvMulti> FQuilkinSocketSubsystem::CreateRecvMulti(int32 MaxNumPackets, int32 MaxPacketSize, ERecvMultiFlags Flags)
{
	return SocketSubsystem->CreateRecvMulti(MaxNumPackets, MaxPacketSize, Flags);
}

TSharedRef<FInternetAddr> FQuilkinSocketSubsystem::GetLocalHostAddr(FOutputDevice& Out, bool& bCanBindAll)
{
	return SocketSubsystem->GetLocalHostAddr(Out, bCanBindAll);
}

bool FQuilkinSocketSubsystem::GetMultihomeAddress(TSharedRef<FInternetAddr>& Addr)
{
	return SocketSubsystem->GetMultihomeAddress(Addr);
}

bool FQuilkinSocketSubsystem::HasNetworkDevice()
{
	return SocketSubsystem->HasNetworkDevice();
}

const TCHAR* FQuilkinSocketSubsystem::GetSocketAPIName() const
{
	return SocketSubsystem->GetSocketAPIName();
}

ESocketErrors FQuilkinSocketSubsystem::GetLastErrorCode()
{
	return SocketSubsystem->GetLastErrorCode();
}

ESocketErrors FQuilkinSocketSubsystem::TranslateErrorCode(int32 Code)
{
	return SocketSubsystem->TranslateErrorCode(Code);
}

bool FQuilkinSocketSubsystem::IsSocketRecvMultiSupported() const
{
	return SocketSubsystem->IsSocketRecvMultiSupported();
}

bool FQuilkinSocketSubsystem::IsSocketWaitSupported() const
{
	return SocketSubsystem->IsSocketWaitSupported();
}

double FQuilkinSocketSubsystem::TranslatePacketTimestamp(const FPacketTimestamp& Timestamp, ETimestampTranslation Translation)
{
	return SocketSubsystem->TranslatePacketTimestamp(Timestamp, Translation);
}

bool FQuilkinSocketSubsystem::IsRecvFromWithPktInfoSupported() const
{
	return SocketSubsystem->IsRecvFromWithPktInfoSupported();
}
