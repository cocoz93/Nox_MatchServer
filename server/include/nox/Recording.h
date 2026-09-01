// Recording.h — 정품 호스트가 실제로 뱉은 SERVER_INFO 원본.
//
// 2026-09-02, 정품 호스트를 띄우고 서버 찾기를 왕복시켜 받은 82바이트다.
// 절반 이상의 자리가 정체 불명이므로, 아는 자리(Token·이름)만 바꿔 쓰고
// 모르는 자리는 원본 그대로 되돌려준다. 이것이 "녹음 재생"이다.
//
// ⚠ 이 배열은 **손으로 고치지 않는다.** 한 번 옮겨 적다가 22번 자리부터
//    한 바이트가 밀려 11개 자리가 어긋난 적이 있다(2026-09-02).
//    바꿔야 하면 캡처 원본에서 기계로 다시 생성할 것.
//
// 자리 대조는 Docs/PROTOCOL.md 「실측 대조」 참조.
//   3(머리) + 69(본문) + 10(이름 "User Game" + 종료) = 82
//
// 원본 (사람이 눈으로 대조할 때 쓰는 정본):
//   00 00 0d 00 1f 0f 0f ff ff ff 53 6f 5f 44 72 75 69 64 00 01 00 00
//   55 00 9a 03 01 00 87 20 03 10 ff ff ff ff ff ff ff ff c0 00 d4 00
//   27 81 48 0b ff ff ff ff ff ef ff ff ff ff ff ff ff ff ff ff ff ff
//   ff ff c0 7a e8 0a 55 73 65 72 20 47 61 6d 65 00
#pragma once

#include <cstddef>
#include <cstdint>

namespace nox {

// 머리 3바이트를 포함한 원본 전체.
inline constexpr uint8_t kRecordedServerInfo[] = {
    0x00, 0x00, 0x0d,                                              // 머리: [0] 스트림0 / [1] 미상(항상 0) / [2] 오프코드 0x0d
    0x00,                                                          // 본문 0   현재 인원
    0x1f,                                                          // 본문 1   최대 인원 = 31 (nox.cfg MaxPlayers 32 - 서버 몫 1)
    0x0f, 0x0f, 0xff, 0xff, 0xff,                                  // 본문 2   미상 5
    0x53, 0x6f, 0x5f, 0x44, 0x72, 0x75, 0x69, 0x64, 0x00,          // 본문 7   맵 이름 "So_Druid" (9바이트 칸)
    0x01,                                                          // 본문 16  상태 1
    0x00,                                                          // 본문 17  상태 2
    0x00, 0x55, 0x00, 0x9a, 0x03, 0x01, 0x00,                      // 본문 18  미상 7
    0x87, 0x20,                                                    // 본문 25  플래그(u16)
    0x03, 0x10,                                                    // 본문 27  미상 2
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,                // 본문 29  미상 8
    0xc0, 0x00, 0xd4, 0x00,                                        // 본문 37  미상 4
    0x27, 0x81, 0x48, 0x0b,                                        // 본문 41  Token — 요청 값을 그대로 돌려준다
    0xff, 0xff, 0xff, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff, 0xff,    // 본문 45  미상 20
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,    //
    0xc0, 0x7a, 0xe8, 0x0a,                                        // 본문 65  미상 4 (67번이 세션마다 변한다)
    0x55, 0x73, 0x65, 0x72, 0x20, 0x47, 0x61, 0x6d, 0x65, 0x00,    // 본문 69  서버 이름 "User Game" + 종료 0
};

inline constexpr size_t kRecordedServerInfoSize = sizeof(kRecordedServerInfo);

// 머리 크기. [0] 신뢰/스트림, [1] 미상(항상 0), [2] 오프코드.
inline constexpr size_t kHeaderSize = 3;

}  // namespace nox
