// 런처 화면 — 본체(launcher_main.cpp)와 폼 꼴 글로 주고받는다
'use strict';
const host = window.chrome && window.chrome.webview;
const $ = (id) => document.getElementById(id);
const send = (obj) => { if (host) host.postMessage(new URLSearchParams(obj).toString()); };

const REASON = {
  wrong_password: '암호가 맞지 않습니다.',
  no_account: '없는 아이디입니다. 가입부터 해 주세요.',
  bad_input: '아이디는 영문 · 숫자 9자까지, 암호는 4~8자입니다.',
  unreachable: '로그인 서버에 닿지 못했습니다. 잠시 뒤 다시 해 주세요.',
  server_error: '로그인 서버 오류입니다. 잠시 뒤 다시 해 주세요.',
  key_invalid: '로그인이 만료되었습니다. 다시 로그인해 주세요.',
  key_store: '이 PC 에 로그인 정보를 저장하지 못했습니다.',
  launch_failed: '게임을 실행하지 못했습니다. 게임 폴더를 확인해 주세요.',
};
const reasonText = (r) => REASON[r] || '알 수 없는 오류입니다(' + (r || '?') + ').';

let me = { name: '', remember: false };

function show(id) {
  for (const p of ['loading', 'login', 'signed']) $(p).classList.toggle('hidden', p !== id);
  if (id === 'login') ($('name').value ? $('password') : $('name')).focus();
}

function showLogin(msg) {
  $('loginMsg').textContent = msg || '';
  $('loginBtn').disabled = false;
  $('password').value = '';
  show('login');
}

function showSigned(note) {
  $('whoName').textContent = me.name;
  $('whoNote').textContent = note || (me.remember ? '자동 로그인 켜짐' : '이번 실행만 로그인');
  $('startMsg').textContent = '';
  $('startBtn').disabled = false;
  show('signed');
}

$('login').addEventListener('submit', (e) => {
  e.preventDefault();
  const name = $('name').value.trim();
  const password = $('password').value;
  if (!name || !password) { $('loginMsg').textContent = '아이디와 암호를 넣어 주세요.'; return; }
  $('loginBtn').disabled = true;
  $('loginMsg').textContent = '로그인 중…';
  send({ cmd: 'login', name, password, remember: $('remember').checked ? '1' : '0', saveId: $('saveId').checked ? '1' : '0' });
  $('password').value = '';
});
$('remember').addEventListener('change', () => { if ($('remember').checked) $('saveId').checked = true; });
$('signup').addEventListener('click', (e) => { e.preventDefault(); send({ cmd: 'signup' }); });
$('startBtn').addEventListener('click', () => {
  $('startBtn').disabled = true;
  $('startMsg').textContent = '게임을 여는 중…';
  send({ cmd: 'start' });
});
$('logout').addEventListener('click', (e) => { e.preventDefault(); send({ cmd: 'logout' }); });

if (host) {
  host.addEventListener('message', (e) => {
    const m = new URLSearchParams(String(e.data));
    switch (m.get('ev')) {
      case 'init':
        $('name').value = m.get('lastName') || '';
        if (m.get('auto') === '1') { me = { name: m.get('lastName'), remember: true }; $('remember').checked = true; showSigned(); }
        else showLogin('');
        break;
      case 'login':
        if (m.get('ok') === '1') { me = { name: m.get('name'), remember: m.get('remember') === '1' }; showSigned(); }
        else showLogin(reasonText(m.get('reason')));
        break;
      case 'start':
        if (m.get('ok') === '1') $('startMsg').textContent = '게임 실행 중';
        else if (m.get('reason') === 'key_invalid') showLogin(reasonText('key_invalid'));
        else { $('startMsg').textContent = reasonText(m.get('reason')); $('startBtn').disabled = false; }
        break;
      case 'gameExit':
        if (m.get('loggedOut') === '1') showLogin('게임이 끝나 로그아웃했습니다.');
        else showSigned();
        break;
      case 'logout':
        $('remember').checked = false;
        showLogin('로그아웃했습니다.');
        break;
      case 'busy':
        break;
    }
  });
  send({ cmd: 'init' });
} else {
  showLogin('런처 밖에서 열었습니다(화면 미리 보기).');
}
