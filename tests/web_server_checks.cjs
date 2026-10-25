const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {spawn}=require('node:child_process'),https=require('node:https'),http=require('node:http'),tls=require('node:tls'),crypto=require('node:crypto');
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const folder=path.resolve(process.argv[3]);fs.mkdirSync(folder,{recursive:true});
const port=Number(process.argv[4]||18888);
const fixture=spawn(process.argv[2],[folder],{windowsHide:true,stdio:['pipe','pipe','pipe']});let output='',error='';fixture.stdout.on('data',x=>output+=x);fixture.stderr.on('data',x=>error+=x);
async function until(test,ms=10000){const end=Date.now()+ms;while(Date.now()<end){if(test())return;if(fixture.exitCode!==null)throw Error(error||'Fixture exited');await wait(20);}throw Error('Web fixture timed out: '+error);}
let ca;
function request(url,options={}){return new Promise((resolve,reject)=>{const client=url.startsWith('https:')?https:http;const req=client.request(url,{ca,...options},res=>{let body='';res.on('data',x=>body+=x);res.on('end',()=>resolve({status:res.statusCode,headers:res.headers,body}));});req.on('error',reject);req.setTimeout(5000,()=>req.destroy(Error('Request timed out')));req.end(options.body);});}
function sendFrame(socket,opcode,value,final=true){
  const payload=Buffer.from(value),mask=crypto.randomBytes(4);
  const header=Buffer.alloc(payload.length<126?2:4);header[0]=(final?128:0)|opcode;
  header[1]=128|(payload.length<126?payload.length:126);if(payload.length>=126)header.writeUInt16BE(payload.length,2);
  for(let i=0;i<payload.length;i++)payload[i]^=mask[i%4];socket.write(Buffer.concat([header,mask,payload]));
}
async function main(){
  await until(()=>output.includes('READY'));ca=new crypto.X509Certificate(fs.readFileSync(path.join(folder,'web-certificate.cer'))).toString();
  const host='127.0.0.1:'+port,origin='https://'+host;
  assert.equal((await request('http://'+host+'/setup')).status,200);
  assert.equal((await request(origin+'/api/state')).status,401);
  assert.equal((await request(origin+'/api/ping')).status,401);
  assert.match((await request(origin+'/')).body,/control-core\.js/);
  assert.match((await request(origin+'/control-core.js')).body,/class PreferenceSaver/);
  const body='code=offline-fixture-passcode';const login=await request(origin+'/api/login',{method:'POST',headers:{Origin:origin,'Content-Type':'application/x-www-form-urlencoded','Content-Length':Buffer.byteLength(body)},body});
  assert.equal(login.status,200);const cookie=login.headers['set-cookie'][0].split(';')[0];assert.match(login.headers['set-cookie'][0],/Secure; HttpOnly; SameSite=Strict/);
  assert.equal((await request(origin+'/api/state',{headers:{Cookie:cookie}})).status,200);
  assert.equal((await request(origin+'/api/ping',{headers:{Cookie:cookie}})).body,'{"ok":true}');
  const socket=tls.connect({host:'127.0.0.1',port,ca});let input=Buffer.alloc(0),upgraded=false,closed=false;const messages=[];socket.on('close',()=>closed=true);socket.on('error',e=>{error+=e.message;});
  socket.on('data',chunk=>{input=Buffer.concat([input,chunk]);if(!upgraded){const end=input.indexOf('\r\n\r\n');if(end<0)return;assert.match(input.subarray(0,end).toString(),/101 Switching Protocols/);input=input.subarray(end+4);upgraded=true;}
    while(input.length>=2){let length=input[1]&127,header=2;if(length===126){if(input.length<4)return;length=input.readUInt16BE(2);header=4;}else if(length===127){if(input.length<10)return;length=Number(input.readBigUInt64BE(2));header=10;}if(input.length<header+length)return;const opcode=input[0]&15,payload=input.subarray(header,header+length);input=input.subarray(header+length);if(opcode===9)sendFrame(socket,10,payload);messages.push(opcode===1?JSON.parse(payload):{binary:opcode,type:payload.length>=4?payload.readUInt32LE(0):0});}});
  await new Promise(resolve=>socket.once('secureConnect',resolve));const key=crypto.randomBytes(16).toString('base64');socket.write(`GET /ws HTTP/1.1\r\nHost: ${host}\r\nOrigin: ${origin}\r\nCookie: ${cookie}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\n\r\n`);
  await until(()=>messages.some(x=>x.type==='state'));await wait(150);assert.equal(messages.some(x=>x.binary),false,'hidden scopes must not stream');
  const send=text=>sendFrame(socket,1,text);
  send('action=subscribe&seq=1&rf=1&audio=0');await until(()=>messages.some(x=>x.binary===2&&x.type===4));assert.equal(messages.some(x=>x.binary===2&&x.type===2),false);
  send('action=subscribe&seq=2&rf=0&audio=1');await until(()=>messages.some(x=>x.binary===2&&x.type===2));
  send('action=fixture&seq=3&generation=1');await until(()=>messages.some(x=>x.type==='ack'&&x.seq==='3'&&x.fixture));
  sendFrame(socket,1,'action=fix',false);sendFrame(socket,9,'between fragments');sendFrame(socket,0,'ture&seq=fragment&generation=1');
  await until(()=>messages.some(x=>x.type==='ack'&&x.seq==='fragment'&&x.fixture));
  assert.ok(messages.some(x=>x.binary===10),'interleaved ping must receive pong');
  for(let i=0;i<45;i++)send('action=fixture&seq=burst-'+i+'&generation=1');
  send('action=ptt&enabled=0&seq=release-burst&generation=1');
  send('action=browserMic&enabled=0&seq=disarm-burst&generation=1');
  await until(()=>messages.some(x=>x.type==='ack'&&x.seq==='disarm-burst'));
  assert.equal(messages.find(x=>x.seq==='release-burst')?.ok,true,'PTT off must bypass the ordinary command burst limit');
  assert.equal(messages.find(x=>x.seq==='disarm-burst')?.ok,true,'mic disarm must bypass the ordinary command burst limit');
  assert.ok(messages.some(x=>x.seq?.startsWith('burst-')&&x.ok===false),'ordinary command burst remains limited');
  await wait(1100);
  send('action=heartbeat');await until(()=>messages.some(x=>x.type==='heartbeat'));
  socket.pause();fixture.stdin.write('FLOOD\n');await until(()=>output.includes('buffered output'),5000);
  send('action=fixture&seq=while-paused&generation=1');
  await until(()=>output.includes('CONTROL while-paused'),1000);
  await wait(2500);fixture.stdin.write('QUIET\n');socket.resume();
  await until(()=>messages.some(x=>x.type==='ack'&&x.seq==='while-paused'&&x.fixture));
  assert.equal(closed,false,'temporary receive congestion must not close the WebSocket');
  await until(()=>output.includes('output resumed after'));
  await until(()=>output.includes('control heartbeat paused'),18000);
  assert.equal(closed,false,'native pongs must keep a JavaScript-paused browser connected');
  assert.ok(output.includes('INACTIVE'),'a stalled page must still release browser PTT/microphone ownership');
  assert.ok(messages.some(x=>x.type==='controlIdle'),'the browser must be told to disarm its local microphone');
  assert.ok(messages.filter(x=>x.binary===9).length>=3,'server must send native keepalive pings');
  send('action=heartbeat');send('action=fixture&seq=resumed&generation=1');
  await until(()=>messages.some(x=>x.type==='ack'&&x.seq==='resumed'&&x.fixture));
  fixture.stdin.write('ROTATE\n');await until(()=>output.includes('ROTATED'));await until(()=>closed);
  assert.equal((await request(origin+'/api/state',{headers:{Cookie:cookie}})).status,401);
  console.log('Web server checks passed: trusted TLS, assets, login/cookie, protected state, WebSocket control/fragmentation, scope subscriptions, slow-reader recovery, native keepalive/application safety deadline, passcode/session invalidation.');
}
main().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>{fixture.stdin.write('STOP\n');fixture.stdin.end();setTimeout(()=>{if(fixture.exitCode===null)fixture.kill();},2000).unref();});
