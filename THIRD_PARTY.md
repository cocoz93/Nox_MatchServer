# 참고한 남의 것

## opennox/libs — MIT

https://github.com/opennox/libs

**무엇을 가져왔나** — 오프코드 242개와 메시지별 바이트 배치.
`noxnet` 패키지에 메시지마다 파일이 하나씩 있고 인코딩·디코딩이 정리되어 있다.

**어떻게 가져왔나** — 구조를 옮긴 것이 아니라 **바이트 배치를 읽어서 C++로 다시 썼다.**
`server/include/nox/Opcode.h`의 이름과 값이 그쪽과 같은 이유는,
캡처 로그·`noxnet`·이 저장소를 같은 이름으로 검색할 수 있어야 하기 때문이다.

**기계로 뽑은 것 하나** (2026-09-05) — `server/src/MsgLen.cpp`, 오프코드별 본문 길이 표다.
242개를 손으로 옮기면 한 자리만 틀려도 패킷을 쪼갤 때 그 뒤가 통째로 밀린다.
그래서 그쪽 `netmsg` 의 등록 결과(`Op.Len()`)를 훑어 C++ 소스로 찍어낸다 —
뽑는 도구가 `tools/dumpscan/genlen.go`(opennox 모듈을 쓰는 Go 도구라 공개 리포에는 없다)이고, 언제든 다시 뽑아 대조할 수 있다.
**옮긴 것은 숫자(사실)이지 코드가 아니다.**

같은 방식으로 뽑은 표가 하나 더 있다 — `server/src/SpellNames.cpp`(주문 번호 ↔ 이름 136개, `spell` 패키지에서 `-genspells`).

MIT는 조건이 하나뿐이다 — 저작권 고지 유지. 아래가 그것이다.

```
MIT License

Copyright (c) 2022 NoX World

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## ⚠ 2026-09-15 정정 — 아래 「쓰지 않는 것」은 **2026-09-12에 뒤집혔다**

원래 이 절은 `opennox/opennox`(GPL-3.0)를 **안 본다**고 못 박고 있었다.
**그 방침은 더 이상 유효하지 않다.** `Docs/SOURCE_POLICY.md`(2026-09-12 확정, 이 프로젝트의 대전제)가
그 저장소를 **정답지로 삼는다**고 정했고, 그 뒤 모든 작업이 그것을 읽으며 이뤄졌다.

    ① Go 를 읽어 뜻을 안다      opennox/src/*.go · src/server/*.go     ← GPL-3.0
    ② C++ 로 구현한다           우리 server/
    ③ C 로 대조한다             opennox/src/legacy/*.c (디컴파일)      ← GPL-3.0 + 원저작권
    ④ 정품 클라 화면으로 확정

**실제로 얼마나 엮였나** (2026-10-29 다시 셈) — `server/` 의 `.h`·`.cpp` 107개(85,845줄) 안에
`<이름>.go` 언급 **828곳(서로 다른 파일 이름 115개)** · `<이름>.c` 언급 **988곳** ·
`sub_501960` 꼴 원본 주소 **489곳**. 주석으로만 남은 것이 아니라 **거기서 값과 순서를 가져왔다.**
세는 법 = 파일 글자에서 꼴을 그대로 센 것(주석·코드 구분 없음). 09-15 에 적었던 수(26,155줄 · 276 · 341 · 869)는
다른 방법으로 센 것이라 이 수와 견주지 않는다.

**그대로 옮긴 표**

| 파일 | 무엇 | 어디서 |
|---|---|---|
| `server/src/core/SoundNames.cpp` | 소리 이름 1,023개(기계로 뽑음) | opennox `src/common/sound/sound.go` 의 `soundNames` — GPL-3.0 |
| `server/include/nox/DirVectors.h` | 방향 256개 → 단위 벡터(비트 그대로) | opennox 에 든 `blob_587000.dat` 194136 — 원래는 `Game.exe` 의 데이터 |
| `GameBalance.h` · `Server.h` · `ThingDb.h` 의 몇 자리 | 회복량 · 이름표 차례 · 소환 마나 같은 값 | 같은 `blob_587000.dat`(자리는 각 파일 주석) |
| `server/src/ThingFlags.cpp` | 종류 번호마다 「복합 오브젝트인가」 비트 하나 | 정품 설치본의 `thing.bin` 에서 기계로 뽑음 — 이름·다른 값은 안 담음 |

### 그래서 지금 상태가 무엇인가

| | |
|---|---|
| **리포에서 뺀 것** (2026-10-28 · 10-29) | opennox 소스를 고친 로컬 기록(`tools/opennox-local/` — GPL 원문이 든 패치) · opennox 모듈을 쓰는 Go 도구(`tools/dumpscan/`) · 게임 데이터 파일 복호 키 표(`server/include/nox/BlowfishTables.h` — 그래서 새로 받은 클론은 그대로는 빌드가 안 된다). 지난 이력에서도 뺐다 |
| **리포에 남은 것** | 서버 C++ 코드. 대부분 읽고 다시 짰지만 **값과 순서를 거기서 가져왔고**, 위 표처럼 그대로 옮긴 것도 있다. 디컴파일 C 와 대조하며 거의 그대로 옮긴 함수도 있다 |
| **라이선스** (2026-10-29) | **GPL-3.0**(`LICENSE`) — 남은 서버 코드가 opennox(GPL-3.0)의 파생이라 같은 라이선스로 낸다 |
| **GPL보다 큰 것** | `src/legacy/*.c` 는 **웨스트우드 Nox 를 디컴파일한 것**이다. 그건 opennox 가 GPL로 넘겨줄 수 있는 권리가 아니다(원저작권은 따로 있다) |

**아래 원래 경고는 그대로 유효하다** — 읽고도 안 지킨 것이 지금 상태라는 뜻이다:

> 디컴파일 코드를 보면 나중에 **"안 봤다"를 증명할 방법이 없다.**
> 저작권 다툼은 *접근했는가*와 *유사한가* 둘로 갈리는데, 본 순간 앞의 하나가 증명되고
> "독자적으로 만들었다"를 내가 증명해야 하는 처지가 된다.

`README.md` 의 **3단계(명세만 보고 재작성)** 가 이 문제를 미리 내다본 설계다 —
"2단계 문서가 1과 3 사이의 방화벽이다." 지금 코드는 **1단계(포팅)** 다(README 「만드는 순서」).

---

## 원래 적어 두었던 것 (2026-09-04 기준 · 위 정정 전)

| | 왜 |
|---|---|
| **`opennox/opennox`** | **GPL-3.0** — 가져다 쓰면 이 저장소 전체가 GPL이 된다. 그리고 저장소 루트의 `cxgo.yml`이 `src/legacy/*.c`를 Go로 기계 변환하는 설정이다. **원본 소스가 공개된 적이 없으니 그 C의 출처는 디컴파일뿐이다** |
| **`nox-decomp`** | 명백한 디컴파일 산출물. 원본 표현이 그대로 남아 파생물로 취급된다 |

당시 규율 — "막혀서 봐야 한다면 봐도 된다. 대신 **본 코드는 옮기지 않는다.**"

---

## 링크하는 라이브러리

- **MySQL 클라이언트**(`libmysql` · 그것이 부르는 `libssl`·`libcrypto`) — 로비·로그인 서버의 DB 연결. 리포에 넣지 않고, 빌드하는 PC 의 MySQL 8.0 설치본에서 링크한다(`CMakeLists.txt` 의 `NOX_MYSQL_DIR`). 없으면 DB 없이 빌드된다
- 나머지는 Windows 기본 라이브러리(`ws2_32` · `bcrypt` · `httpapi` 등)뿐이다

---

## 게임 클라이언트

**GOG에서 정식 구매한 설치본만 쓴다.** 저장소에 넣지 않는다.
절차는 `Docs/CLIENT_SETUP.md`에 적어둔다.
