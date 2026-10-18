#pragma once
// ★ 10-19 MO 5단계 작업 3 — 판 결과 기록기(`Docs/MATCH_RESULT_DESIGN.md`).
//
// 방이 지은 판 결과(`MatchResult` — 방 이벤트 `MATCH_RESULT`)를 판마다 한 줄(JSON Lines)로 파일에 덧붙인다.
//   · 게임 스레드는 저장을 기다리지 않는다 — 줄을 지어 큐에 넣고 깨우기만 한다. 쓰기는 기록 스레드가 한다
//     (목표는 DB — 느린 바깥 호출이라 처음부터 따로 · 원칙 「게임 스레드는 저장을 기다리지 않음」)
//   · 쓰기가 실패해도 판은 계속 — 셈만 오른다(`Stats`)
//   · 끝낼 때(`Stop`) 큐에 남은 줄을 다 쓰고 스레드를 닫는다
//
// 한 줄의 칸 — 판 번호 `match`(<epoch>-<방>-<판>) · 방 · 방 이름 · 맵 · 모드 · 시작/끝 유닉스 초 · 프레임 · 초 ·
//   끝 까닭(`endReason` — 갈래 일곱 + `aborted`) · 승자(`kind` none/player/team/draw · `id` · 사람이면 `account`) · 점수 한도 ·
//   시간 한도(초) · 사람마다(로비 이름 — 없으면 null · `netCode` · 팀 · 점수 · 죽은 수 · `state` finished/left) · 팀마다(번호 · 점수 · 깃발 득점)
// 문자열의 비 ASCII — UTF-8 로 맞으면 그대로, 아니면 바이트마다 `\u00XX`(로비 이름의 바이트가 무엇인지 미확인 — 문서 「미확인」)

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "nox/RoomManager.h"

namespace nox {

std::string FormatMatchResultJson(const MatchResult& r);   // 줄바꿈 없는 한 줄
std::string FormatMatchResultLine(const MatchResult& r);   // 사람이 읽는 `[판 결과] …` 한 줄(줄바꿈 없음)
std::string MatchIdOf(const MatchResult& r);                // <epoch>-<방>-<판>

class CMatchRecorder {
public:
    struct Stats {
        unsigned long long pushed = 0;       // 받은 판 결과
        unsigned long long written = 0;      // 파일에 쓴 줄
        unsigned long long writeFailed = 0;  // 못 쓴 줄(열기 · 쓰기 실패)
    };

    explicit CMatchRecorder(std::string path) : _path(std::move(path)) {}
    ~CMatchRecorder() { Stop(); }
    CMatchRecorder(const CMatchRecorder&) = delete;
    CMatchRecorder& operator=(const CMatchRecorder&) = delete;

    // 폴더를 만들고 파일을 덧붙이기로 열어 본 뒤 기록 스레드를 띄운다. 못 열면 false(스레드는 그래도 띄운다 — 쓸 때마다 다시 열어 보고 못 쓰면 셈)
    bool Start();
    // 게임 스레드 — 줄을 지어 큐에 넣고 깨운다(기다리지 않는다)
    void Push(const MatchResult& r);
    // 큐를 다 쓰고 스레드를 닫는다(두 번 불러도 됨)
    void Stop();
    Stats GetStats() const;
    const std::string& Path() const { return _path; }

private:
    void Run();
    bool WriteLine(const std::string& line);

    std::string _path;
    std::thread _thread;
    mutable std::mutex _mu;
    std::condition_variable _cv;
    std::deque<std::string> _queue;
    bool _stop = false;
    bool _started = false;
    std::atomic<unsigned long long> _pushed{0};
    std::atomic<unsigned long long> _written{0};
    std::atomic<unsigned long long> _writeFailed{0};
};

}  // namespace nox
