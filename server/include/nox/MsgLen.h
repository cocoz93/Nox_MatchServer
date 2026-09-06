// MsgLen.h — 오프코드별 본문 길이. 패킷 하나에서 메시지를 쪼갤 때 쓴다.
//
// 왜 필요한가 — 한 UDP 패킷 안에 [오프코드][본문] 이 여러 개 이어 붙는다.
// 길이를 모르면 어디서 끊기는지 알 수 없고, 한 번 어긋나면 그 뒤가 통째로 밀린다.
//
// 표 자체는 MsgLen.cpp 에 있고 **기계로 뽑는다** (tools/dumpscan -genlen).
// 손으로 고치지 말 것.
#pragma once

#include <cstdint>

namespace nox {

// 색인 = 오프코드. 값 = 오프코드를 뺀 본문 길이. -1 이면 가변 또는 미상.
extern const int16_t kMsgBodyLen[256];

// 방향이 정해져야 길이가 정해지는 것들(0xAA·0xAB)을 가려서 돌려준다.
// 그 밖의 오프코드는 위 표를 그대로 돌려준다.
int MsgBodyLenByDir(uint8_t op, bool fromClient);

}  // namespace nox
