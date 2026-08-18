#include "stdafx.h"
#include "NetworkStatistics.h"
#include "TCPServer.h"

namespace
{
	constexpr auto NETWORK_STATISTICS_INTERVAL = std::chrono::seconds(1);

	const char* GetSendPacketName(std::uint8_t head)
	{
		switch (static_cast<SOCKET_STATE>(head))
		{
		case SOCKET_STATE::SEND_ID: return "ID";
		case SOCKET_STATE::SEND_UPDATE_DATA: return "UPDATE_DATA";
		case SOCKET_STATE::SEND_NUM_OF_CLIENT: return "NUM_OF_CLIENT";
		case SOCKET_STATE::SEND_BLUE_SUIT_WIN: return "BLUE_SUIT_WIN";
		case SOCKET_STATE::SEND_ZOMBIE_WIN: return "ZOMBIE_WIN";
		case SOCKET_STATE::SEND_GAME_START: return "GAME_START";
		case SOCKET_STATE::SEND_CHANGE_SLOT: return "CHANGE_SLOT";
		case SOCKET_STATE::SEND_OPEN_DRAWER_SOUND: return "OPEN_DRAWER_SOUND";
		case SOCKET_STATE::SEND_CLOSE_DRAWER_SOUND: return "CLOSE_DRAWER_SOUND";
		case SOCKET_STATE::SEND_OPEN_DOOR_SOUND: return "OPEN_DOOR_SOUND";
		case SOCKET_STATE::SEND_CLOSE_DOOR_SOUND: return "CLOSE_DOOR_SOUND";
		case SOCKET_STATE::SEND_BLUE_SUIT_DEAD: return "BLUE_SUIT_DEAD";
		case SOCKET_STATE::SEND_SPACEOUT_OBJECTS: return "SPACEOUT_OBJECTS";
		case SOCKET_STATE::SEND_LOADING_COMPLETE: return "LOADING_COMPLETE";
		default: return "UNKNOWN";
		}
	}

	const char* GetReceivePacketName(std::uint8_t head)
	{
		switch (static_cast<ReceiveHead>(head))
		{
		case ReceiveHead::KeysBuffer: return "KEYS_BUFFER";
		case ReceiveHead::GameStart: return "GAME_START";
		case ReceiveHead::ChangeSlot: return "CHANGE_SLOT";
		case ReceiveHead::LoadingComplete: return "LOADING_COMPLETE";
		default: return "UNKNOWN";
		}
	}

	template<class Operation>
	void ForEachStatistics(
		NetworkStatistics& total,
		NetworkStatistics& interval,
		Operation operation)
	{
		operation(total);
		operation(interval);
	}

	void AccumulateNetworkStatistics(NetworkStatistics& destination, const NetworkStatistics& source)
	{
		destination.sentBytes += source.sentBytes;
		destination.receivedBytes += source.receivedBytes;
		destination.sentPackets += source.sentPackets;
		destination.receivedPackets += source.receivedPackets;
		destination.sendWouldBlockCount += source.sendWouldBlockCount;
		destination.receiveWouldBlockCount += source.receiveWouldBlockCount;
		destination.peakUnsentBytes = (std::max)(destination.peakUnsentBytes, source.peakUnsentBytes);
		destination.peakPendingPackets = (std::max)(destination.peakPendingPackets, source.peakPendingPackets);

		for (std::size_t i = 0; i < NetworkStatistics::PACKET_TYPE_COUNT; ++i)
		{
			destination.sentByHead[i].bytes += source.sentByHead[i].bytes;
			destination.sentByHead[i].packets += source.sentByHead[i].packets;
			destination.receivedByHead[i].bytes += source.receivedByHead[i].bytes;
			destination.receivedByHead[i].packets += source.receivedByHead[i].packets;
		}
	}

	void PrintPacketBreakdown(
		const char* direction,
		const std::array<NetworkPacketStatistics, NetworkStatistics::PACKET_TYPE_COUNT>& statistics,
		bool isSend)
	{
		bool hasPacket = false;
		std::cout << "  " << direction << " by head:";
		for (std::size_t i = 0; i < statistics.size(); ++i)
		{
			if (statistics[i].bytes == 0 && statistics[i].packets == 0)
			{
				continue;
			}

			hasPacket = true;
			const char* packetName = isSend
				? GetSendPacketName(static_cast<std::uint8_t>(i))
				: GetReceivePacketName(static_cast<std::uint8_t>(i));
			std::cout << ' ' << packetName << '=' << statistics[i].bytes
				<< "B/" << statistics[i].packets << "pkt";
		}

		if (!hasPacket)
		{
			std::cout << " none";
		}
		std::cout << '\n';
	}
}

void SocketNetworkStatistics::RecordQueueState(std::size_t unsentBytes, std::size_t pendingPackets)
{
	ForEachStatistics(mTotal, mInterval, [=](NetworkStatistics& statistics)
		{
			statistics.peakUnsentBytes = (std::max)(statistics.peakUnsentBytes, unsentBytes);
			statistics.peakPendingPackets = (std::max)(statistics.peakPendingPackets, pendingPackets);
		});
}

void SocketNetworkStatistics::RecordSentBytes(std::uint8_t head, std::size_t byteCount)
{
	ForEachStatistics(mTotal, mInterval, [=](NetworkStatistics& statistics)
		{
			statistics.sentBytes += byteCount;
			statistics.sentByHead[head].bytes += byteCount;
		});
}

void SocketNetworkStatistics::RecordSentPacket(std::uint8_t head)
{
	ForEachStatistics(mTotal, mInterval, [=](NetworkStatistics& statistics)
		{
			++statistics.sentPackets;
			++statistics.sentByHead[head].packets;
		});
}

void SocketNetworkStatistics::RecordSendWouldBlock()
{
	ForEachStatistics(mTotal, mInterval, [](NetworkStatistics& statistics)
		{
			++statistics.sendWouldBlockCount;
		});
}

void SocketNetworkStatistics::RecordReceivedBytes(std::size_t byteCount)
{
	ForEachStatistics(mTotal, mInterval, [=](NetworkStatistics& statistics)
		{
			statistics.receivedBytes += byteCount;
		});
}

void SocketNetworkStatistics::RecordReceivedPacket(std::uint8_t head, std::size_t byteCount)
{
	ForEachStatistics(mTotal, mInterval, [=](NetworkStatistics& statistics)
		{
			++statistics.receivedPackets;
			++statistics.receivedByHead[head].packets;
			statistics.receivedByHead[head].bytes += byteCount;
		});
}

void SocketNetworkStatistics::RecordReceiveWouldBlock()
{
	ForEachStatistics(mTotal, mInterval, [](NetworkStatistics& statistics)
		{
			++statistics.receiveWouldBlockCount;
		});
}

void SocketNetworkStatistics::ResetInterval(std::size_t unsentBytes, std::size_t pendingPackets)
{
	mInterval = NetworkStatistics{};
	mInterval.peakUnsentBytes = unsentBytes;
	mInterval.peakPendingPackets = pendingPackets;
}

ServerNetworkStatisticsReporter::ServerNetworkStatisticsReporter()
	: mLastReportTime(std::chrono::steady_clock::now())
{}

void ServerNetworkStatisticsReporter::ReportIfDue(
	NetworkClientStatisticsView* clients,
	std::size_t clientCount)
{
	const auto currentTime = std::chrono::steady_clock::now();
	const auto elapsedTime = currentTime - mLastReportTime;
	if (elapsedTime < NETWORK_STATISTICS_INTERVAL)
	{
		return;
	}
	mLastReportTime = currentTime;

	if (clientCount == 0)
	{
		return;
	}

	NetworkStatistics aggregateStatistics;
	std::size_t currentUnsentBytes = 0;
	std::size_t currentPendingPackets = 0;
	for (std::size_t i = 0; i < clientCount; ++i)
	{
		const NetworkClientStatisticsView& client = clients[i];
		AccumulateNetworkStatistics(aggregateStatistics, client.statistics->GetInterval());
		currentUnsentBytes += client.unsentBytes;
		currentPendingPackets += client.pendingPackets;
	}

	const auto elapsedMilliseconds =
		std::chrono::duration_cast<std::chrono::milliseconds>(elapsedTime).count();
	std::cout << "[NET " << elapsedMilliseconds << "ms] TX="
		<< aggregateStatistics.sentBytes << "B/"
		<< aggregateStatistics.sentPackets << "pkt, RX="
		<< aggregateStatistics.receivedBytes << "B/"
		<< aggregateStatistics.receivedPackets << "pkt, Queue="
		<< currentUnsentBytes << "B/" << currentPendingPackets
		<< "pkt, MaxClientPeak=" << aggregateStatistics.peakUnsentBytes << "B/"
		<< aggregateStatistics.peakPendingPackets << "pkt, WouldBlock(TX/RX)="
		<< aggregateStatistics.sendWouldBlockCount << '/'
		<< aggregateStatistics.receiveWouldBlockCount << '\n';
	PrintPacketBreakdown("TX", aggregateStatistics.sentByHead, true);
	PrintPacketBreakdown("RX", aggregateStatistics.receivedByHead, false);

	for (std::size_t i = 0; i < clientCount; ++i)
	{
		NetworkClientStatisticsView& client = clients[i];
		const NetworkStatistics& statistics = client.statistics->GetInterval();
		std::cout << "  client[" << client.clientIndex << "]: TX=" << statistics.sentBytes
			<< "B/" << statistics.sentPackets << "pkt, RX="
			<< statistics.receivedBytes << "B/" << statistics.receivedPackets
			<< "pkt, Queue=" << client.unsentBytes << "B/"
			<< client.pendingPackets << "pkt, Peak="
			<< statistics.peakUnsentBytes << "B/"
			<< statistics.peakPendingPackets << "pkt\n";

		client.statistics->ResetInterval(client.unsentBytes, client.pendingPackets);
	}
}

void ServerNetworkStatisticsReporter::ReportDisconnected(
	int clientIndex,
	const SocketNetworkStatistics& socketStatistics) const
{
	const NetworkStatistics& statistics = socketStatistics.GetTotal();
	std::cout << "[NET END client=" << clientIndex << "] TX="
		<< statistics.sentBytes << "B/" << statistics.sentPackets
		<< "pkt, RX=" << statistics.receivedBytes << "B/"
		<< statistics.receivedPackets << "pkt, PeakQueue="
		<< statistics.peakUnsentBytes << "B/" << statistics.peakPendingPackets
		<< "pkt, WouldBlock(TX/RX)=" << statistics.sendWouldBlockCount
		<< '/' << statistics.receiveWouldBlockCount << '\n';
	PrintPacketBreakdown("TX total", statistics.sentByHead, true);
	PrintPacketBreakdown("RX total", statistics.receivedByHead, false);
}
