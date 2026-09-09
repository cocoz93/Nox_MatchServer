// NoxCrypt.h — 녹스 게임 데이터 복호. **독립 서버 P1 의 바닥돌이다.**
//
// 녹스의 .bin·.map 은 **Blowfish ECB** 로 잠겨 있다 (opennox/noxcrypt 대조, 2026-09-10).
//   · 8바이트 블록 단위, 패딩 없음(파일 크기가 8의 배수)
//   · 키 = 896바이트 표에서 28*i 자리부터 56바이트 (BlowfishTables.h 의 kNoxKeys)
//   · thing.bin = 키 7 · .map = 키 19 · gamedata.bin = 키 8
//
// ⚠ 원본 파일은 **읽기만** 한다 — 복호는 메모리에서.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nox {

// Blowfish 한 벌 — 표준 알고리즘, 표는 BlowfishTables.h.
class Blowfish {
public:
    // key 는 1~56바이트. 녹스는 늘 56을 쓴다.
    void Init(const uint8_t* key, size_t keyLen);
    void DecryptBlock(uint8_t* b8) const;   // 8바이트 제자리 복호
    void EncryptBlock(uint8_t* b8) const;   // 왕복 검증용

private:
    uint32_t Feistel(uint32_t x) const;
    void EncryptWords(uint32_t& l, uint32_t& r) const;
    uint32_t p_[18];
    uint32_t s_[4][256];
};

// 녹스 키 번호. noxcrypt 의 상수 그대로.
enum : int {
    kKeySoundSet = 5,
    kKeyThingBin = 7,
    kKeyGameData = 8,
    kKeyModifier = 13,
    kKeyMap      = 19,
    kKeyMonster  = 23,
    kKeySave     = 27,
};

// 파일을 통째로 읽어 복호해 돌려준다. 실패면 빈 벡터.
//   ⚠ 크기가 8의 배수가 아니면 **꼬리는 버린다** — noxcrypt 의 Reader 가 마지막
//     모자란 블록에서 ErrUnexpectedEOF 를 내는 것과 같은 규칙이다(reader.go readNext).
std::vector<uint8_t> DecryptFile(const std::string& path, int keyIndex);

}  // namespace nox
