const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {spawn}=require('node:child_process'),https=require('node:https'),http=require('node:http'),tls=require('node:tls'),crypto=require('node:crypto');
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const folder=path.resolve(process.argv[3]);fs.mkdirSync(folder,{recursive:true});
const fixture=spawn(process.argv[2],[folder],{windowsHide:true,stdio:['pipe','pipe','pipe']});let output='',error='';fixture.stdout.on('data',x=>output+=x);fixture.stderr.on('data',x=>error+=x);
async function until(test,ms=10000){const end=Date.now()+ms;while(Date.now()<end){if(test())return;if(fixture.exitCode!==null)throw Error(error||'Fixture exited');await wait(20);}throw Error('Web fixture timed out: '+error);}
let ca;
function request(url,options={}){return new Promise((resolve,reject)=>{const client=url.startsWith('https:')?https:http;const req=client.request(url,{ca,...options},res=>{let body='';res.on('data',x=>body+=x);res.on('end',()=>resolve({status:res.statusCode,headers:res.headers,body}));});req.on('error',reject);req.setTimeout(5000,()=>req.destroy(Error('Request timed out')));req.end(options.body);});}
async function main(){
  await until(()=>output.includes('READY'));ca=new crypto.X509Certificate(fs.readFileSync(path.join(folder,'web-certificate.cer'))).toString();
  const host='127.0.0.1:8888',origin='https://'+host;
  assert.equal((await request('http://'+host+'/setup')).status,200);
  assert.equal((await request(origin+'/api/state')).status,401);
  assert.match((await request(origin+'/')).body,/control-core\.js/);
  assert.match((await request(origin+'/control-core.js')).body,/class PreferenceSaver/);
  const body='code=offline-fixture-passcode';const login=await request(origin+'/api/login',{method:'POST',headers:{Origin:origin,'Content-Type':'application/x-www-form-urlencoded','Content-Length':Buffer.byteLength(body)},body});
  assert.equal(login.status,200);const cookie=login.headers['set-cookie'][0].split(';')[0];assert.match(login.headers['set-cookie'][0],/Secure; HttpOnly; SameSite=Strict/);
  assert.equal((await request(origin+'/api/state',{headers:{Cookie:cookie}})).status,200);
  const socket=tls.connect({host:'127.0.0.1',port:8888,ca});let input=Buffer.alloc(0),upgraded=false,closed=false;const messages=[];socket.on('close',()=>closed=true);socket.on('error',e=>{error+=e.message;});
  socket.on('data',chunk=>{input=Buffer.concat([input,chunk]);if(!upgraded){const end=input.indexOf('\r\n\r\n');if(end<0)return;assert.match(input.subarray(0,end).toString(),/101 Switching Protocols/);input=input.subarray(end+4);upgraded=true;}
    while(input.length>=2){let length=input[1]&127,header=2;if(length===126){if(input.length<4)return;length=input.readUInt16BE(2);header=4;}else if(length===127){if(input.length<10)return;length=Number(input.readBigUInt64BE(2));header=10;}if(input.length<header+length)return;const opcode=input[0]&15,payload=input.subarray(header,header+length);input=input.subarray(header+length);messages.push(opcode===1?JSON.parse(payload):{binary:opcode,type:payload.length>=4?payload.readUInt32LE(0):0});}});
  await new Promise(resolve=>socket.once('secureConnect',resolve));const key=crypto.randomBytes(16).toString('base64');socket.write(`GET /ws HTTP/1.1\r\nHost: ${host}\r\nOrigin: ${origin}\r\nCookie: ${cookie}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\n\r\n`);
  await until(()=>messages.some(x=>x.type==='state'));await wait(150);assert.equal(messages.some(x=>x.binary),false,'hidden scopes must not stream');
  const send=text=>{const payload=Buffer.from(text),mask=crypto.randomBytes(4),header=Buffer.from([0x81,0x80|payload.length]);assert.ok(payload.length<126);for(let i=0;i<payload.length;i++)payload[i]^=mask[i%4];socket.write(Buffer.concat([header,mask,payload]));};
  send('action=subscribe&seq=1&rf=1&audio=0');await until(()=>messages.some(x=>x.binary===2&&x.type===4));assert.equal(messages.some(x=>x.binary===2&&x.type===2),false);
  send('action=subscribe&seq=2&rf=0&audio=1');await until(()=>messages.some(x=>x.binary===2&&x.type===2));
  send('action=fixture&seq=3&generation=1');await until(()=>messages.some(x=>x.type==='ack'&&x.seq==='3'&&x.fixture));
  fixture.stdin.write('ROTATE\n');await until(()=>output.includes('ROTATED'));await until(()=>closed);
  assert.equal((await request(origin+'/api/state',{headers:{Cookie:cookie}})).status,401);
  console.log('Web server checks passed: trusted TLS, assets, login/cookie, protected state, WebSocket control, scope subscriptions, passcode/session invalidation.');
}
main().catch(e=>{console.error(e);process.exitCode=1;}).finally(()=>{fixture.stdin.write('STOP\n');fixture.stdin.end();setTimeout(()=>{if(fixture.exitCode===null)fixture.kill();},2000).unref();});
