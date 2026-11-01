// 런처 화면의 정품 모양(런처 P10 · 사용자 시안 B4 + T1) — 640x480 그림판 하나에 그리고 창에 정수배로 키운다.
//   그림 · 글꼴은 본체(NoxArt.h)가 게임 폴더에서 뽑아 https://art.nox/ 에 둔 것. 못 읽으면 load() 가 false — 이때는 app.css 의 정품 그림 없는 모양 그대로.
//   글자는 정품 default.fnt 비트맵으로 찍는다. 입력칸 · 단추 · 링크는 원래 HTML 요소를 그림판 위 같은 자리에 투명하게 얹어
//   키보드 · 한글 입력 · 탭 이동은 브라우저에 맡기고, 보이는 것만 여기서 그린다(app.js 는 요소 상태만 만진다).
'use strict';
const stage = (() => {
  const ART = 'https://art.nox/';
  const W = 640, H = 480;
  const GOLD = 'rgb(240,176,48)', BRIGHT = 'rgb(255,214,110)', VALUE = 'rgb(232,222,200)', DIM = 'rgb(190,160,100)', BAD = 'rgb(255,130,100)', OFF = 'rgb(150,135,110)';
  const STRIP_W = 342;                   // 판 폭의 기준(시안의 위 띠 폭)
  const PANEL = { x: (W - STRIP_W) / 2 - 2, y: 172, w: STRIP_W + 4 };
  const FIELD_X = PANEL.x + 40, FIELD_W = PANEL.w - 80;
  const BTN_W = 198, BTN_H = 40;

  const $ = (id) => document.getElementById(id);
  let ctx = null, base = null, font = null;
  const img = {};
  let dirty = false, caretOn = true;

  // ── 글꼴(default.fnt — 머리 28바이트 · 구간표 · 글자마다 [폭 1바이트][비트맵]). 형식은 opennox/libs noxfont
  function parseFont(buf) {
    const v = new DataView(buf), bytes = new Uint8Array(buf);
    if (String.fromCharCode(bytes[3], bytes[2], bytes[1], bytes[0]) !== 'FoNt') throw new Error('font magic');
    const stride = v.getUint32(12, true), height = v.getUint32(16, true), count = v.getUint32(20, true), size = v.getUint32(24, true);
    const ranges = [];
    for (let i = 0; i < count; i++) {
      const p = 28 + 8 * i;
      ranges.push({ off: v.getUint32(p, true), start: v.getUint16(p + 4, true), end: v.getUint16(p + 6, true) });
    }
    const data = 28 + 8 * count;
    const glyph = (c) => {
      for (const r of ranges) if (c >= r.start && c <= r.end) return data + size * (r.off + c - r.start);
      return -1;
    };
    if (glyph(0xc544) < 0) throw new Error('font has no hangul');   // 「아」 — 한글 없는 글꼴이면 라벨이 비므로 기본 모양으로
    return { bytes, stride, height, glyph };
  }

  const glyphAt = (c) => {
    let at = font.glyph(c);
    if (at < 0) at = font.glyph(0xfffd);
    return at;
  };

  function textWidth(s) {
    let w = 0;
    for (let i = 0; i < s.length; i++) {
      const at = glyphAt(s.charCodeAt(i));
      w += at < 0 ? 4 : font.bytes[at] + 1;
    }
    return w;
  }

  function stamp(g, s, x, y, color) {
    g.fillStyle = color;
    for (let i = 0; i < s.length; i++) {
      const at = glyphAt(s.charCodeAt(i));
      if (at < 0) { x += 4; continue; }
      const w = font.bytes[at];
      for (let row = 0; row < font.height; row++) {
        const line = at + 1 + row * font.stride;
        for (let col = 0; col < w; col++) {
          if ((font.bytes[line + (col >> 3)] >> (7 - (col & 7))) & 1) g.fillRect(x + col, y + row, 1, 1);
        }
      }
      x += w + 1;
    }
  }

  // 글자 + 오른쪽 아래 1픽셀 그림자(정품 메뉴 글자처럼)
  function text(s, x, y, color) {
    stamp(ctx, s, x + 1, y + 1, '#000');
    stamp(ctx, s, x, y, color);
  }
  // 정품 글꼴에 「…」이 없어 점 셋으로
  const centered = (raw, y, color) => {
    const s = raw.replace(/…/g, '...');
    text(s, PANEL.x + Math.floor((PANEL.w - textWidth(s)) / 2), y, color);
  };

  // ── 그림
  async function loadBitmap(name) {
    const res = await fetch(ART + name + '.png');
    if (!res.ok) throw new Error(name);
    return createImageBitmap(await res.blob());
  }

  function newCanvas(w, h) {
    const c = document.createElement('canvas');
    c.width = w;
    c.height = h;
    return c;
  }

  // 정품 저작권 화면에서 NOX 글자만 — 불빛 · 갈색 바탕은 (빨강-파랑)/최댓값이 1 가까움, 쇠 글자는 0 이하
  function keyedLogo() {
    const src = newCanvas(W, H), g = src.getContext('2d');
    g.drawImage(img.CopyrightScreen, 0, 0);
    const im = g.getImageData(0, 0, W, H), d = im.data;
    let minX = W, minY = H, maxX = 0, maxY = 0;
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const p = (y * W + x) * 4, r = d[p], gr = d[p + 1], b = d[p + 2], mx = Math.max(r, gr, b);
        const ratio = mx > 0 ? (r - b) / mx : 1;
        let a = Math.min(1, Math.max(0, (0.62 - ratio) / 0.18));
        if (r + gr + b < 45) a = 0;                                               // 까만 바탕
        const dx = x - 287, dy = y - 231;
        if (dx * dx + dy * dy < 1600) a = 0;                                      // O 가운데 해
        if (x >= 30 && x < 85 && y >= 135 && y < 172) a = 0;                      // 왼쪽 위 불빛 점
        d[p + 3] = Math.round(a * 255);
        if (a > 0.6) {
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
          if (y < minY) minY = y;
          if (y > maxY) maxY = y;
        }
      }
    }
    g.putImageData(im, 0, 0);
    return { canvas: src, x: minX, y: minY, w: maxX - minX + 1, h: maxY - minY + 1 };
  }

  // 가장자리로 갈수록 옅어지는 검정(글자 뒤 그늘)
  function softDark(g, x0, y0, x1, y1, feather, maxA) {
    const im = g.getImageData(x0, y0, x1 - x0, y1 - y0), d = im.data, w = x1 - x0, h = y1 - y0;
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const e = Math.min(x, w - 1 - x, y, h - 1 - y);
        let a = maxA;
        if (e < feather) {
          const t = e / feather;
          a *= t * t * (3 - 2 * t);
        }
        const p = (y * w + x) * 4;
        d[p] *= 1 - a;
        d[p + 1] *= 1 - a;
        d[p + 2] *= 1 - a;
      }
    }
    g.putImageData(im, x0, y0);
  }

  function buildBase() {
    base = newCanvas(W, H);
    const g = base.getContext('2d', { willReadFrequently: true });
    g.drawImage(img.WolapiLoginBG, 0, 0);
    const logo = keyedLogo();
    const h = 142, w = Math.floor(logo.w * h / logo.h), lx = Math.floor((W - w) / 2), ly = 22;
    softDark(g, lx - 50, ly - 6, lx + w + 50, ly + h + 6, 40, 0.6);
    g.imageSmoothingEnabled = true;
    g.imageSmoothingQuality = 'high';
    g.drawImage(logo.canvas, logo.x, logo.y, logo.w, logo.h, lx, ly, w, h);
  }

  function tile(bmp, x0, y0, x1, y1) {
    for (let y = y0; y < y1; y += bmp.height) {
      for (let x = x0; x < x1; x += bmp.width) {
        const w = Math.min(bmp.width, x1 - x), h = Math.min(bmp.height, y1 - y);
        ctx.drawImage(bmp, 0, 0, w, h, x, y, w, h);
      }
    }
  }

  // 정품 쇠 테두리(모서리 13x13 · 가로 20x6 · 세로 6x20)
  function ironFrame(x0, y0, x1, y1) {
    const c = 13;
    tile(img.BorderHorizontal, x0 + c, y0, x1 - c, y0 + 6);
    tile(img.BorderHorizontal, x0 + c, y1 - 6, x1 - c, y1);
    tile(img.BorderVertical, x0, y0 + c, x0 + 6, y1 - c);
    tile(img.BorderVertical, x1 - 6, y0 + c, x1, y1 - c);
    ctx.drawImage(img.BorderCornerUL, x0, y0);
    ctx.drawImage(img.BorderCornerUR, x1 - c, y0);
    ctx.drawImage(img.BorderCornerLL, x0, y1 - c);
    ctx.drawImage(img.BorderCornerLR, x1 - c, y1 - c);
  }

  function place(el, x, y, w, h) {
    const s = el.style;
    s.left = x + 'px';
    s.top = y + 'px';
    s.width = w + 'px';
    s.height = h + 'px';
  }

  function drawField(el, x, y, w, h, shown) {
    place(el, x, y, w, h);
    ctx.fillStyle = 'rgb(10,7,4)';
    ctx.fillRect(x, y, w, h);
    const focused = document.activeElement === el;
    ctx.strokeStyle = focused ? 'rgb(150,112,50)' : 'rgb(70,70,74)';
    ctx.lineWidth = 1;
    ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);
    ctx.fillStyle = '#000';
    ctx.fillRect(x + 1, y + 1, w - 2, 1);
    const tx = x + 6, ty = y + 4;
    if (focused && el.selectionStart !== el.selectionEnd) {
      const a = textWidth(shown.slice(0, el.selectionStart)), b = textWidth(shown.slice(0, el.selectionEnd));
      ctx.fillStyle = 'rgb(110,80,30)';
      ctx.fillRect(tx + a - 1, y + 2, b - a + 1, h - 4);
    }
    text(shown, tx, ty, VALUE);
    if (focused && caretOn && el.selectionStart === el.selectionEnd) {
      ctx.fillStyle = VALUE;
      ctx.fillRect(tx + textWidth(shown.slice(0, el.selectionEnd)), y + 3, 1, h - 6);
    }
  }

  function drawCheck(input, x, y, label) {
    const lbl = input.parentElement;
    place(lbl, x - 2, y - 1, 16 + textWidth(label) + 3, font.height + 3);
    ctx.drawImage(input.checked ? img.UICheckBoxLit : img.UICheckBox, x, y + 1);
    text(label, x + 16, y, document.activeElement === input ? BRIGHT : GOLD);
    return x + 16 + textWidth(label);
  }

  function drawButton(el, y, label) {
    const x = PANEL.x + Math.floor((PANEL.w - BTN_W) / 2);
    place(el, x, y, BTN_W, BTN_H);
    const active = el.matches(':hover') || document.activeElement === el;
    ctx.drawImage(el.disabled ? img.UIButtonHugeDis : active ? img.UIButtonHugeLit : img.UIButtonHuge, x, y);
    text(label, x + Math.floor((BTN_W - textWidth(label)) / 2), y + Math.floor((BTN_H - font.height) / 2), el.disabled ? OFF : BRIGHT);
  }

  function drawLink(el, x, y, label) {
    const w = textWidth(label);
    place(el, x - 2, y - 1, w + 4, font.height + 4);
    const color = el.matches(':hover') || document.activeElement === el ? 'rgb(255,240,190)' : BRIGHT;
    text(label, x, y, color);
    ctx.fillStyle = color;
    ctx.fillRect(x, y + font.height + 1, w, 1);
  }

  const msgColor = (el) => (el.dataset.info === '1' ? DIM : BAD);

  // 안쪽을 먼저 재서(메시지가 있으면 판이 한 줄 길어짐) 판 → 내용 차례로
  function panelBack(bottom) {
    ctx.fillStyle = 'rgba(0,0,0,0.6667)';
    ctx.fillRect(PANEL.x, PANEL.y, PANEL.w, bottom - PANEL.y);
    ironFrame(PANEL.x - 3, PANEL.y - 3, PANEL.x + PANEL.w + 3, bottom + 3);
  }

  function drawLogin() {
    const h = font.height, msg = $('loginMsg').textContent;
    panelBack(msg ? 410 : 394);
    let y = PANEL.y + 18;
    text('아이디', FIELD_X, y, GOLD);
    y += h + 4;
    drawField($('name'), FIELD_X, y, FIELD_W, h + 8, $('name').value);
    y += h + 18;
    text('암호', FIELD_X, y, GOLD);
    y += h + 4;
    drawField($('password'), FIELD_X, y, FIELD_W, h + 8, '*'.repeat($('password').value.length));
    y += h + 18;
    const after = drawCheck($('saveId'), FIELD_X, y, '아이디 저장');
    drawCheck($('remember'), after + 18, y, '자동 로그인');
    y += h + 14;
    drawButton($('loginBtn'), y, '로그인');
    y += BTN_H + 14;
    if (msg) {
      centered(msg, y, msgColor($('loginMsg')));
      y += h + 5;
    }
    const a = '계정이 없나요?', gap = textWidth('  '), b = '가입하기';
    const fx = PANEL.x + Math.floor((PANEL.w - textWidth(a) - gap - textWidth(b)) / 2);
    text(a, fx, y, DIM);
    drawLink($('signup'), fx + textWidth(a) + gap, y, b);
  }

  function drawSigned() {
    const h = font.height, msg = $('startMsg').textContent;
    panelBack(msg ? 346 : 328);
    let y = PANEL.y + 22;
    centered($('whoName').textContent + ' 님', y, BRIGHT);
    y += h + 7;
    centered($('whoNote').textContent, y, DIM);
    y += h + 16;
    drawButton($('startBtn'), y, '게임 시작');
    y += BTN_H + 14;
    if (msg) {
      centered(msg, y, msgColor($('startMsg')));
      y += h + 5;
    }
    const b = '로그아웃';
    drawLink($('logout'), PANEL.x + Math.floor((PANEL.w - textWidth(b)) / 2), y, b);
  }

  function paint() {
    dirty = false;
    ctx.drawImage(base, 0, 0);
    if (!$('login').classList.contains('hidden')) drawLogin();
    else if (!$('signed').classList.contains('hidden')) drawSigned();
    else {
      panelBack(240);
      centered('불러오는 중…', 200, DIM);
    }
  }

  function invalidate() {
    if (dirty || !ctx) return;
    dirty = true;
    requestAnimationFrame(paint);
  }

  function fit() {
    $('frame').style.transform = 'scale(' + (window.innerWidth / W) + ')';
    invalidate();
  }

  // 투명 입력칸의 글자 폭은 정품 글꼴과 달라, 누른 자리는 정품 글꼴 기준으로 다시 잡는다
  function caretFromClick(e) {
    const el = e.target;
    if (el.selectionStart !== el.selectionEnd) return;
    const shown = el.type === 'password' ? '*'.repeat(el.value.length) : el.value;
    const scale = window.innerWidth / W, x = (e.clientX / scale) - parseFloat(el.style.left) - 6;
    let best = 0;
    for (let i = 1; i <= shown.length; i++) if (textWidth(shown.slice(0, i)) <= x + 2) best = i;
    el.setSelectionRange(best, best);
    invalidate();
  }

  function wire() {
    new MutationObserver(invalidate).observe($('frame'), { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ['class', 'disabled'] });
    for (const ev of ['input', 'change', 'focusin', 'focusout', 'pointerover', 'pointerout', 'keyup', 'selectionchange']) document.addEventListener(ev, invalidate);
    window.addEventListener('resize', fit);
    for (const id of ['name', 'password']) $(id).addEventListener('mouseup', caretFromClick);
    setInterval(() => {
      const f = document.activeElement;
      caretOn = !caretOn;
      if (f && (f.id === 'name' || f.id === 'password')) invalidate();
    }, 500);
  }

  async function load() {
    try {
      const names = ['WolapiLoginBG', 'CopyrightScreen', 'UICheckBox', 'UICheckBoxLit', 'UIButtonHuge', 'UIButtonHugeLit', 'UIButtonHugeDis',
                     'BorderCornerUL', 'BorderCornerUR', 'BorderCornerLL', 'BorderCornerLR', 'BorderHorizontal', 'BorderVertical'];
      const [bitmaps, fnt] = await Promise.all([
        Promise.all(names.map(loadBitmap)),
        fetch(ART + 'default.fnt').then((r) => { if (!r.ok) throw new Error('font'); return r.arrayBuffer(); }),
      ]);
      names.forEach((n, i) => { img[n] = bitmaps[i]; });
      font = parseFont(fnt);
      buildBase();
      const c = $('stage');
      ctx = c.getContext('2d');
      document.body.classList.add('art');
      wire();
      fit();
      return true;
    } catch (e) {
      console.warn('정품 그림 없음 — 기본 모양으로', e);
      return false;
    }
  }

  return { load };
})();
