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

**실제로 얼마나 엮였나** (09-15 전수 집계) — 우리 서버 26,155줄 안에
정답지 `.go` 인용 **276곳(서로 다른 93개 파일)** · 디컴파일 C 인용 **341곳** ·
`sub_501960` 같은 원본 주소 **869곳**. 주석으로만 남은 것이 아니라 **거기서 값과 순서를 가져왔다.**

**기계로 뽑은 것 하나** — `server/src/core/SoundNames.cpp`(소리 이름 1,023개)는
`opennox-src/src/common/sound/sound.go` 의 `soundNames` 에서 그대로 뽑았다.

### 그래서 지금 상태가 무엇인가

| | |
|---|---|
| **리포에서 뺀 것** (2026-10-28) | GPL 원문이 든 파일 — opennox 소스를 고친 로컬 기록(`tools/opennox-local/`) · opennox 모듈을 쓰는 Go 도구(`tools/dumpscan/`). 지난 이력에서도 뺐다 |
| **리포에 남은 것** | 서버 C++ 코드. 원문을 옮긴 것이 아니라 읽고 다시 짰지만, **값과 순서를 거기서 가져왔다**(위 집계) |
| **공개하면** | 남은 서버 코드가 GPL 파생으로 보이면 GPL-3.0 조건이 붙는다 — 같은 라이선스로, 출처를 밝히고, 소스와 함께. 이 리포는 아직 LICENSE 를 정하지 않았다 |
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

## 게임 클라이언트

**GOG에서 정식 구매한 설치본만 쓴다.** 저장소에 넣지 않는다.
절차는 `Docs/CLIENT_SETUP.md`에 적어둔다.
