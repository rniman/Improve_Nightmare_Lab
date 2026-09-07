#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

// 특정 패킷 HEAD의 애플리케이션 데이터량과 완성 패킷 수다.
struct NetworkPacketStatistics
{
	std::uint64_t bytes = 0;
	std::uint64_t packets = 0;
};

// bytes는 TCP/IP 헤더를 제외한 애플리케이션 데이터량이며,
// packets는 partial I/O가 모두 끝난 완성 패킷만 집계한다.
struct NetworkStatistics
{
	static constexpr std::size_t PACKET_TYPE_COUNT = 256;

	std::uint64_t sentBytes = 0;
	std::uint64_t receivedBytes = 0;
	std::uint64_t sentPackets = 0;
	std::uint64_t receivedPackets = 0;

	// 대기 데이터량이 아니라 WSAEWOULDBLOCK이 발생한 호출 횟수다.
	std::uint64_t sendWouldBlockCount = 0;
	std::uint64_t receiveWouldBlockCount = 0;

	// 측정 구간 중 송신 큐 잔여량의 최고치다. 단위: byte/packet.
	std::size_t peakUnsentBytes = 0;
	std::size_t peakPendingPackets = 0;

	// 배열 인덱스는 wire packet의 uint8_t HEAD 값이다.
	std::array<NetworkPacketStatistics, PACKET_TYPE_COUNT> sentByHead = {};
	std::array<NetworkPacketStatistics, PACKET_TYPE_COUNT> receivedByHead = {};
};

// 한 연결의 전체 수명 누적값과 현재 출력 구간 값을 항상 함께 갱신한다.
class SocketNetworkStatistics
{
public:
	SocketNetworkStatistics();
	SocketNetworkStatistics(SocketNetworkStatistics&&) noexcept = default;
	SocketNetworkStatistics& operator=(SocketNetworkStatistics&&) noexcept = default;
	SocketNetworkStatistics(const SocketNetworkStatistics&) = delete;
	SocketNetworkStatistics& operator=(const SocketNetworkStatistics&) = delete;

	// 송신 버퍼를 큐에 추가한 직후 현재 backlog를 최고치에 반영한다.
	void RecordQueueState(std::size_t unsentBytes, std::size_t pendingPackets);

	// Bytes는 실제 send()/recv() 결과를, Packet은 패킷 완성 시점만 기록한다.
	void RecordSentBytes(std::uint8_t head, std::size_t byteCount);
	void RecordSentPacket(std::uint8_t head);
	void RecordSendWouldBlock();
	void RecordReceivedBytes(std::size_t byteCount);
	void RecordReceivedPacket(std::uint8_t head, std::size_t byteCount);
	void RecordReceiveWouldBlock();

	const NetworkStatistics& GetTotal() const { return mStorage->total; }
	const NetworkStatistics& GetInterval() const { return mStorage->interval; }

	// 현재 backlog를 시작값으로 삼아 다음 출력 구간을 시작한다.
	void ResetInterval(std::size_t unsentBytes, std::size_t pendingPackets);

private:
	struct Storage
	{
		NetworkStatistics total;    // 연결 전체 수명 누적값
		NetworkStatistics interval; // 마지막 주기 출력 이후의 값
	};

	std::unique_ptr<Storage> mStorage;
};

// TCPServer가 통계 리포터에 전달하는 비소유 연결별 스냅샷이다.
struct NetworkClientStatisticsView
{
	std::size_t clientIndex = 0;
	// 리포트 요청 시점의 송신 큐 잔여량이다.
	std::size_t unsentBytes = 0;
	std::size_t pendingPackets = 0;
	// 비소유 포인터이며 ReportIfDue() 호출이 끝날 때까지 유효해야 한다.
	SocketNetworkStatistics* statistics = nullptr;
};

// 서버 전체 통계의 1초 출력 주기, 연결별 집계와 콘솔 출력 형식을 관리한다.
class ServerNetworkStatisticsReporter
{
public:
	ServerNetworkStatisticsReporter();

	// 1초마다 활성 연결 통계를 집계·출력하고 구간 통계를 초기화한다.
	void ReportIfDue(NetworkClientStatisticsView* clients, std::size_t clientCount);

	// ClientConnectionState 초기화 전에 해당 연결의 전체 누적 통계를 출력한다.
	void ReportDisconnected(int clientIndex, const SocketNetworkStatistics& statistics) const;

private:
	// 시스템 시각 변경이나 게임 타이머 정지의 영향을 받지 않는 마지막 출력 시각이다.
	std::chrono::steady_clock::time_point mLastReportTime;
};
