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
뽑는 도구가 `tools/dumpscan/genlen.go` 이고, 언제든 다시 뽑아 대조할 수 있다.
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

## 쓰지 않는 것

| | 왜 |
|---|---|
| **`opennox/opennox`** | **GPL-3.0** — 가져다 쓰면 이 저장소 전체가 GPL이 된다. 그리고 저장소 루트의 `cxgo.yml`이 `src/legacy/*.c`를 Go로 기계 변환하는 설정이다. **원본 소스가 공개된 적이 없으니 그 C의 출처는 디컴파일뿐이다** |
| **`nox-decomp`** | 명백한 디컴파일 산출물. 원본 표현이 그대로 남아 파생물로 취급된다 |

디컴파일 코드를 보면 나중에 **"안 봤다"를 증명할 방법이 없다.**
저작권 다툼은 *접근했는가*와 *유사한가* 둘로 갈리는데, 본 순간 앞의 하나가 증명되고
"독자적으로 만들었다"를 내가 증명해야 하는 처지가 된다.

막혀서 봐야 한다면 봐도 된다. 대신 **본 코드는 옮기지 않는다.**

---

## 게임 클라이언트

**GOG에서 정식 구매한 설치본만 쓴다.** 저장소에 넣지 않는다.
절차는 `Docs/CLIENT_SETUP.md`에 적어둔다.
