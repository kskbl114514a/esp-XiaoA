/* ---------------------------------------------------------------
 * ESP_SHM v4 writer  (injected into the game bundle head)
 *   JS collects player data (camp/aiLevel/liveState/hp/name/pos/velocity),
 *   does worldToScreen inside the game, packs ShmHeader(esp_data.bin) and
 *   lets libesp.so's ImGui layer draw boxes WITHOUT any view matrix.
 *
 * Layout MUST stay in sync with jni/src/shm_abi.h (asserted by shm_abi_test.py).
 * 下面一律是「相对文件头 0 字节」的**绝对偏移**，与下方 VO_*/O_* 常量逐字对应：
 *   ShmHeader: magic u32@0 version u32@4 seq u64@8 view@16 count i32@104 pad@108 ents@112
 *   ShmView  : viewProj[16]@16 camPos[3]@80 fovY@92 viewW@96 viewH@100  (size 88, 起于 16)
 *   ShmEntity: id u64@0 head[3]@8 chest[3]@20 pelvis[3]@32 feet[3]@44 vel[3]@56
 *              hp@68 maxHp@72 dist@76 flags u32@80 name[32]@84 hasBones u8@116
 *              screenSpace u8@117 pad[2]@118 bones[15][3]@120 sHead[2]@300
 *              sChest[2]@308 sFeet[2]@316 sLead[2]@324 leadM@332 dropM@336 tFly@340
 *              camp i32@344 aiLevel i32@348 liveState i32@352 pad@356
 *              (size 360)
 * --------------------------------------------------------------- */
;globalThis.__ESPSHM__=(function(){var G=globalThis;
var CFG=G.__ESP_CFG__||{
  on:!0,            // master switch
  intervalMs:33,    // write throttle (ms) -> ~30Hz, smooth enough for boxes
  maxDist:400,      // drop entities farther than this (meters)
  lead:!0,          // also write bullet-track lead point (sLead/flags bit4)
  bulletSpeed:900,  // m/s, used by the lead solver
  gravity:9.8,      // m/s^2
  onlyReal:!1,      // v5：不再在 JS 侧过滤 AI，改由 ImGui 侧 hideAI 开关决定
  skipDead:!1,      // v5：不再在 JS 侧过滤倒地/死亡，改由 ImGui 侧 hideKnocked 开关决定
  maxEnts:64
};
G.__ESP_CFG__=CFG;
if(!G.__AIM_CFG__)G.__AIM_CFG__={on:!0,head:!1,maxDist:300,fov:70,alwaysHit:!0};

var MAGIC=0x45535031,VER=5;
// 注意：ENT_SZ 必须等于 C++ 侧 sizeof(ShmEntity)，含 4 字节尾部对齐填充（360 而非 356），
// 否则从第 2 个实体起 stride 漂移 4 字节，读端全部错位。
var HDR_SZ=23152,ENT0=112,ENT_SZ=360,OFF_COUNT=104;
var O_ID=0,O_HEAD=8,O_CHEST=20,O_PELVIS=32,O_FEET=44,O_VEL=56;
var O_HP=68,O_MAXHP=72,O_DIST=76,O_FLAGS=80,O_NAME=84,O_HB=116,O_SS=117;
var O_SH=300,O_SC=308,O_SF=316,O_SL=324,O_LM=332,O_DM=336,O_FT=340;
// v5 追加：原始判定字段（透传给 ImGui 侧做「真人/敌人/AI/倒地」过滤）
var O_CAMP=344,O_AI=348,O_LS=352;
var VO_MVP=16,VO_CAMPOS=80,VO_FOV=92,VO_W=96,VO_H=100;
var F_VISIBLE=1,F_TEAM=2,F_DEAD=4,F_SCREEN=8,F_LEAD=16;

/* ---------------- buffer ---------------- */
var buf=null,dv=null,u8=null;
function ensure(){ if(!buf){ buf=new ArrayBuffer(HDR_SZ); dv=new DataView(buf); u8=new Uint8Array(buf); } }
var seq=0;

/* ---------------- io ---------------- */
function wdir(){
  try{ if(typeof native!=="undefined"&&native.fileUtils&&native.fileUtils.getWritablePath)return native.fileUtils.getWritablePath(); }catch(e){}
  try{ if(typeof jsb!=="undefined"&&jsb.fileUtils&&jsb.fileUtils.getWritablePath)return jsb.fileUtils.getWritablePath(); }catch(e){}
  return "";
}
var ioTag="",ioSz=-1;
// Cocos Creator 3.8 签名：writeDataToFile(data: ArrayBuffer, filePath: string) —— data 在前！
// 旧代码 (path,u8) 会被 jsb_conversions_spec.cpp 拒绝：Convert parameter to Data failed!
// 且 C++ 层只打 E 日志、不抛 JS 异常 -> return !0 谎报成功，文件从未被创建。
function _fsz(fu,q){ try{ if(fu&&fu.getFileSize)return fu.getFileSize(q)|0; }catch(e){} return -1; }
function wbin(path){
  var fu=null;
  try{ if(typeof native!=="undefined"&&native.fileUtils)fu=native.fileUtils; }catch(e){}
  if(!fu){ try{ if(typeof jsb!=="undefined"&&jsb.fileUtils)fu=jsb.fileUtils; }catch(e){} }
  if(!fu||!fu.writeDataToFile){ ioTag="none"; return !1; }
  var ok=!1;
  try{ fu.writeDataToFile(buf,path); ok=!0; ioTag="ab"; }catch(e){ ioTag="ab:err"; }
  if(!ok){ try{ fu.writeDataToFile(u8,path); ok=!0; ioTag="u8"; }catch(e){ ioTag="u8:err"; } }
  ioSz=_fsz(fu,path);
  if(ioSz!==HDR_SZ)return !1;
  return ok;
}

/* ---------------- math / cc helpers ---------------- */
var V3=null,V2=null,haveCC=!1;
function ccInit(){
  if(haveCC)return !0;
  try{ if(typeof cc!=="undefined"){ V3=cc.Vec3; V2=cc.Vec2; haveCC=!!(V3&&V2); } }catch(e){}
  return haveCC;
}
var _p3=null,_v2o=null;
function projW2S(cam,x,y,z,vw,vh,out){
  try{
    if(!ccInit())return !1;
    if(!_p3)_p3=new V3();
    if(!_v2o)_v2o=new V2();
    _p3.x=x;_p3.y=y;_p3.z=z;
    _v2o.x=0;_v2o.y=0;
    cam.worldToScreen(_v2o,_p3);
    if(!isFinite(_v2o.x)||!isFinite(_v2o.y))return !1;
    out[0]=_v2o.x/vw;
    out[1]=_v2o.y/vh;      // cocos: origin bottom-left, y up
    return !0;
  }catch(e){ return !1; }
}
var FLIP=1,FLIPN=0;   // FLIP 不再一次性固化：相机/画布方向变化后每 300 帧重判一次

function pickCam(logic){
  try{
    var me=logic&&logic.getSelfEntity&&logic.getSelfEntity();
    var pc=me&&me.RenderNodeCom&&me.RenderNodeCom.playerComponent;
    var c=pc&&pc.canvas&&pc.canvas.camera;
    if(c&&c.worldToScreen)return c;
  }catch(e){}
  try{ if(typeof cc!=="undefined"&&cc.Camera&&cc.Camera.main&&cc.Camera.main.worldToScreen)return cc.Camera.main; }catch(e){}
  return null;
}
function camPos(cam){
  try{ var n=cam.node; if(n&&n.worldPosition)return n.worldPosition; }catch(e){}
  // 回退链：部分版本 cam.node.worldPosition 尚未刷新或为 (0,0,0)，依次尝试
  try{ var n2=cam.node; if(n2&&typeof n2.getWorldPosition==="function"){var w=n2.getWorldPosition();if(w&&isFinite(w.x))return w;} }catch(e){}
  try{ var n3=cam.node; if(n3&&n3.position&&isFinite(n3.position.x))return n3.position; }catch(e){}
  try{ var cc3=cc&&cc.Camera&&cc.Camera.main; if(cc3&&cc3.node&&cc3.node.worldPosition)return cc3.node.worldPosition; }catch(e){}
  try{ var p=cam.cameraPosition; if(p&&isFinite(p.x))return p; }catch(e){}
  return null;
}
function camWH(cam){
  var w=0,h=0;
  try{ w=cam.width|0; h=cam.height|0; }catch(e){}
  if(!w||!h){ try{ var fs=cc.view.getFrameSize(); w=fs.width|0; h=fs.height|0; }catch(e){} }
  if(!w||!h){ w=1920; h=1080; }
  return [w,h];
}
function camFov(cam){ try{ var f=cam.fov; if(f>1&&f<170)return f; }catch(e){} return 60; }

/* ---------------- lead solver (mirrors jni/src/bullet_track.cpp Solve) ---------------- */
function solveLead(cam,camP,vw,vh,tgt,vel,v,g,out){
  var t=Math.sqrt((tgt.x-camP.x)*(tgt.x-camP.x)+(tgt.y-camP.y)*(tgt.y-camP.y)+(tgt.z-camP.z)*(tgt.z-camP.z))/v;
  if(!(t>0)||t>3)t=3;
  var px=tgt.x,py=tgt.y,pz=tgt.z;
  for(var i=0;i<4;i++){
    px=tgt.x+vel.x*t;
    py=tgt.y+vel.y*t-0.5*g*t*t;
    pz=tgt.z+vel.z*t;
    var dx=px-camP.x,dy=py-camP.y,dz=pz-camP.z;
    var nt=Math.sqrt(dx*dx+dy*dy+dz*dz)/v;
    if(!(nt>0))break;
    if(Math.abs(nt-t)<1e-4){ t=nt; break; }
    t=nt;
  }
  out.drop=0.5*g*t*t;
  out.t=t;
  if(!projW2S(cam,px,py,pz,vw,vh,out.s))return !1;
  out.wx=px;out.wy=py;out.wz=pz;
  return !0;
}

/* ---------------- pack one entity ---------------- */
var _t4=[0,0],_t5=[0,0],_t6=[0,0],_ld={s:[0,0],drop:0,t:0,wx:0,wy:0,wz:0};
var _vel={x:0,y:0,z:0};

function packEnt(base,o,cam,camP,vw,vh,selfCamp,selfId,dist){
  var e=o.t,d=o.d;
  dv.setUint32(base+O_ID,o.id>>>0,!0);          // low 32 bits (ids fit)
  dv.setUint32(base+O_ID+4,0,!0);
  var f=o.feet,h=o.head,c=o.chest,p=o.pelvis;
  dv.setFloat32(base+O_HEAD,f32(h.x),!0);dv.setFloat32(base+O_HEAD+4,f32(h.y),!0);dv.setFloat32(base+O_HEAD+8,f32(h.z),!0);
  dv.setFloat32(base+O_CHEST,f32(c.x),!0);dv.setFloat32(base+O_CHEST+4,f32(c.y),!0);dv.setFloat32(base+O_CHEST+8,f32(c.z),!0);
  dv.setFloat32(base+O_PELVIS,f32(p.x),!0);dv.setFloat32(base+O_PELVIS+4,f32(p.y),!0);dv.setFloat32(base+O_PELVIS+8,f32(p.z),!0);
  dv.setFloat32(base+O_FEET,f32(f.x),!0);dv.setFloat32(base+O_FEET+4,f32(f.y),!0);dv.setFloat32(base+O_FEET+8,f32(f.z),!0);
  dv.setFloat32(base+O_VEL,f32(o.vel.x),!0);dv.setFloat32(base+O_VEL+4,f32(o.vel.y),!0);dv.setFloat32(base+O_VEL+8,f32(o.vel.z),!0);
  dv.setFloat32(base+O_HP,f32(d.hp),!0);
  dv.setFloat32(base+O_MAXHP,f32(d.maxHp),!0);
  dv.setFloat32(base+O_DIST,f32(dist),!0);

  var flags=0;
  var rc=(o.rawCamp!==void 0&&o.rawCamp!==null&&isFinite(o.rawCamp))?o.rawCamp:d.camp;
  var sc=(selfCamp!==void 0&&selfCamp!==null&&isFinite(selfCamp))?selfCamp:0;
  if(rc!==void 0&&sc&&rc===sc)flags|=F_TEAM;
  if(d.liveState===1)flags|=F_DEAD;

  // screen coords (0..1, origin top-left) -- read side multiplies by viewport
  var okH=projW2S(cam,h.x,h.y,h.z,vw,vh,_t4);
  var okF=projW2S(cam,f.x,f.y,f.z,vw,vh,_t5);
  var okC=projW2S(cam,c.x,c.y,c.z,vw,vh,_t6);
  if(okH&&okF){
    // cocos y-up: head raw y > feet raw y => 需要翻转（常规情况）。
    // 原实现只在第一帧判一次并永久固化，首帧若取到观战/加载相机，后面整局 y 都是反的。
    FLIPN++;
    if(FLIPN===1||(FLIPN%300)===0){
      var want=(_t4[1]>_t5[1])?1:0;
      if(want!==FLIP){
        FLIP=want;
        try{ console.warn("[ESPSHM] yflip="+FLIP+" n="+FLIPN+" headY="+_t4[1].toFixed(3)+" footY="+_t5[1].toFixed(3)); }catch(e){}
      }
    }
  }
  if(FLIP===1){ if(okH)_t4[1]=1-_t4[1]; if(okF)_t5[1]=1-_t5[1]; if(okC)_t6[1]=1-_t6[1]; }

  if(okH&&okF){
    flags|=F_SCREEN;
    dv.setUint8(base+O_SS,1);
    dv.setFloat32(base+O_SH,f32(_t4[0]),!0);dv.setFloat32(base+O_SH+4,f32(_t4[1]),!0);
    dv.setFloat32(base+O_SF,f32(_t5[0]),!0);dv.setFloat32(base+O_SF+4,f32(_t5[1]),!0);
    if(okC){dv.setFloat32(base+O_SC,f32(_t6[0]),!0);dv.setFloat32(base+O_SC+4,f32(_t6[1]),!0);}
    else   {dv.setFloat32(base+O_SC,f32((_t4[0]+_t5[0])*0.5),!0);dv.setFloat32(base+O_SC+4,f32((_t4[1]+_t5[1])*0.5),!0);}
  }else{
    // buffer 是复用的：不清零就残留**上一个实体**的 sChest/sHead，
    // 读端一旦按别的条件取用，画出来的胸点属于别人。
    dv.setUint8(base+O_SS,0);
    dv.setFloat32(base+O_SH,0,!0);dv.setFloat32(base+O_SH+4,0,!0);
    dv.setFloat32(base+O_SC,0,!0);dv.setFloat32(base+O_SC+4,0,!0);
    dv.setFloat32(base+O_SF,0,!0);dv.setFloat32(base+O_SF+4,0,!0);
  }
  // 提前量字段同理：本帧不解算就必须清掉，否则 flags 没置 F_LEAD、数值却是上一个人的
  if(!(CFG.lead&&camP)){ flags&=~F_LEAD;
    dv.setFloat32(base+O_SL,0,!0);dv.setFloat32(base+O_SL+4,0,!0);
    dv.setFloat32(base+O_LM,0,!0);dv.setFloat32(base+O_DM,0,!0);dv.setFloat32(base+O_FT,0,!0); }

  // bullet-track lead: solve with velocity, project the lead point
  if(CFG.lead&&camP&&dist<=Math.max(CFG.bulletSpeed,50)*3){
    _vel.x=f32(o.vel.x);_vel.y=f32(o.vel.y);_vel.z=f32(o.vel.z);
    var tg={x:h.x,y:h.y,z:h.z};
    if(solveLead(cam,camP,vw,vh,tg,_vel,Math.max(50,CFG.bulletSpeed||900),CFG.gravity||0,_ld)){
      var lx=_ld.s[0],ly=_ld.s[1];
      if(FLIP===1)ly=1-ly;
      // 与 C++ Solve().leadMeters 同语义：|vel| * 飞行时间（提前量位移长度，米）。
      // 原写法拿「相机→预测点距离 − 相机→脚距离」的绝对值冒充，量纲对、数值不是一回事。
      var vmag=Math.sqrt(_vel.x*_vel.x+_vel.y*_vel.y+_vel.z*_vel.z);
      var lm=vmag*_ld.t;
      flags|=F_LEAD;
      dv.setFloat32(base+O_SL,f32(lx),!0);dv.setFloat32(base+O_SL+4,f32(ly),!0);
      dv.setFloat32(base+O_LM,f32(lm),!0);
      dv.setFloat32(base+O_DM,f32(_ld.drop),!0);
      dv.setFloat32(base+O_FT,f32(_ld.t),!0);
    }
  }

  dv.setUint32(base+O_FLAGS,flags,!0);
  // JS 侧不产骨骼（bones[15] 恒 0），显式写 hasBones=0，
  // 读端据此走「只有头/胸/脚」的退化分支，不会去画一坨 (0,0,0) 的假骨骼。
  dv.setUint8(base+O_HB,0);
  var nm=o.name;
  for(var i=0;i<32;i++)dv.setUint8(base+O_NAME+i,0);
  // 名字按 UTF-8 写入（原写法把 >127 的字符统统一脚踩成 '?'，中文昵称在 ImGui 上就是 "????????"）
  var bytes=utf8(nm,31);
  for(i=0;i<bytes.length;i++)dv.setUint8(base+O_NAME+i,bytes[i]);

  // ---- v5：把 JS 判定用的原始字段透传给 ImGui 侧（-1 表示游戏未提供）
  var cm=o.rawCamp,ai=o.rawAi,ls=o.rawLive;
  dv.setInt32(base+O_CAMP,(cm===void 0||cm===null||!isFinite(cm))?-1:(cm|0),!0);
  dv.setInt32(base+O_AI,(ai===void 0||ai===null||!isFinite(ai))?-1:(ai|0),!0);
  dv.setInt32(base+O_LS,(ls===void 0||ls===null||!isFinite(ls))?-1:(ls|0),!0);
}

function f32(v){
  if(typeof v!=="number"||!isFinite(v))return 0;
  return v;
}
var _uenc=null;
// 写 name[32] 的 UTF-8 编码器：Cocos 的 JS 是 UTF-16，直接取 charCodeAt 写字节只能得到 ASCII
function utf8(str,cap){
  var out=[];
  if(!str)return out;
  if(typeof TextEncoder!=="undefined"){
    if(!_uenc)_uenc=new TextEncoder();
    var b=_uenc.encode(""+str);
    for(var i=0;i<b.length&&out.length<cap;i++)out.push(b[i]);
    return out;
  }
  for(var j=0;j<str.length&&out.length<cap;j++){
    var c=str.charCodeAt(j);
    if(c<0x80)out.push(c);
    else if(c<0x800){out.push(0xc0|(c>>6),0x80|(c&0x3f));}
    else if(c<0xd800||c>0xdfff){out.push(0xe0|(c>>12),0x80|((c>>6)&0x3f),0x80|(c&0x3f));}
    else out.push(0x3f);
  }
  return out;
}
function num(v){ return (typeof v==="number"&&isFinite(v))?v:0; }
function vec3ish(p){ if(!p)return null; if(!isFinite(p.x)||!isFinite(p.y)||!isFinite(p.z))return null; return p; }

/* ---------------- main collector (overrides the txt writer) ---------------- */
G.__ESP_COL__=function(logic,list,S,R){try{
  if(CFG.on===!1)return;
  var now=Date.now();
  if(now-(G.__ESP_T0__||0)<(CFG.intervalMs||33))return;
  G.__ESP_T0__=now;
  ensure();

  var cli=(S&&S.Client!==void 0)?S.Client:0;
  var cam=pickCam(logic);
  if(!cam){ G.__ESPSHM_N__=(G.__ESPSHM_N__||0)+1; return; }
  var wh=camWH(cam),vw=wh[0],vh=wh[1];
  var camP=vec3ish(camPos(cam));

  var selfId=0,selfCamp=0;
  try{ var me=logic&&logic.getSelfEntity&&logic.getSelfEntity();
       if(me&&me.CharacterCom){ selfId=me.CharacterCom.id||0;
         var md=me.CharacterCom.data; if(md)selfCamp=md.camp||0; } }catch(e){}

  // ---- header (write data first, seq last -> reader checks even & stable)
  dv.setUint32(VO_MVP,0,!0);
  for(var k=0;k<15;k++)dv.setFloat32(VO_MVP+4*(k+1),0,!0);
  if(camP){dv.setFloat32(VO_CAMPOS,f32(camP.x),!0);dv.setFloat32(VO_CAMPOS+4,f32(camP.y),!0);dv.setFloat32(VO_CAMPOS+8,f32(camP.z),!0);}
  dv.setFloat32(VO_FOV,f32(camFov(cam)),!0);
  dv.setUint32(VO_W,vw,!0);dv.setUint32(VO_H,vh,!0);

  var n=0,maxE=CFG.maxEnts||64;
  for(var key in list){
    if(n>=maxE)break;
    // for-in 会带出原型链属性：少了这行，同一实体可能被采集多次，白占 maxEnts 名额
    if(!Object.prototype.hasOwnProperty.call(list,key))continue;
    try{
      var t=list[key];
      if(!t||!t.CharacterCom)continue;
      var com=t.CharacterCom;
      if(com.mode!==cli)continue;
      var d=com.data; if(!d)continue;
      var id=(com.id!==void 0?com.id:d.id)|0;
      if(!id||id===selfId)continue;
      // 只跳过「确认死亡」（liveState==1）。hp<=0 但还没死亡（倒地/濒死）必须写出去，
      // 否则 ImGui 侧的 hideKnocked 永远收不到 liveState==2 的实体，开关形同虚设。
      if(d.liveState===1)continue;
      if(CFG.skipDead&&d.liveState!==void 0&&d.liveState!==0)continue;
      if(d.hp<=0&&d.liveState===void 0)continue;
      if(CFG.onlyReal&&d.aiLevel!==void 0&&d.aiLevel>0)continue;

      var mv=t.Movement&&t.Movement.state&&t.Movement.state.position;
      var nd=t.RenderNodeCom&&t.RenderNodeCom.node;
      var feet=vec3ish(mv)||vec3ish(nd&&nd.worldPosition);
      if(!feet)continue;
      var pc=t.RenderNodeCom&&t.RenderNodeCom.playerComponent;
      var headP=pc&&pc.headTrackNode&&vec3ish(pc.headTrackNode.worldPosition);
      var bodyP=pc&&pc.bodyTrackNode&&vec3ish(pc.bodyTrackNode.worldPosition);
      var head=headP||{x:feet.x,y:feet.y+1.72,z:feet.z};
      var chest=bodyP||{x:feet.x,y:feet.y+1.25,z:feet.z};
      var pelvis={x:feet.x,y:feet.y+0.95,z:feet.z};

      var dist=0;
      if(camP){var dx=feet.x-camP.x,dy=feet.y-camP.y,dz=feet.z-camP.z;dist=Math.sqrt(dx*dx+dy*dy+dz*dz);}
      if(dist>CFG.maxDist)continue;

      var vel={x:0,y:0,z:0};
      try{var sv=t.Movement.state.velocity;if(sv){vel.x=num(sv.x);vel.y=num(sv.y);vel.z=num(sv.z);}}catch(e){}

      var nm=d.name||d.nickName||d.nick||d.roleName||d.playerName||"";
      nm=(""+nm).replace(/[\r\n]/g," ");
      var base=ENT0+n*ENT_SZ;
      packEnt(base,{id:id,t:t,d:d,feet:feet,head:head,chest:chest,pelvis:pelvis,name:nm,vel:vel,
                    rawCamp:(d.camp!==void 0?d.camp:(d.teamId!==void 0?d.teamId:(com.teamId!==void 0?com.teamId:void 0))),
                    rawAi:d.aiLevel,rawLive:d.liveState},
              cam,camP,vw,vh,selfCamp,selfId,dist);
      ++n;
    }catch(e1){}
  }
  dv.setUint32(OFF_COUNT,n,!0);
  dv.setUint32(0,MAGIC,!0);
  dv.setUint32(4,VER,!0);
  seq=(seq+2)>>>0;
  dv.setUint32(8,seq,!0);
  dv.setUint32(12,0,!0);

  var dir=wdir(),ok=!1;
  if(!dir)ioTag="nodir";                     // 拿不到可写目录就直写相对路径 = 落到游戏 CWD，基本必失败
  else ok=wbin(dir+"esp_data.bin");
  G.__ESPSHM_N__=(G.__ESPSHM_N__||0)+1;
  if(G.__ESPSHM_N__===1||G.__ESPSHM_N__%60===0){
    try{console.warn("[ESPSHM] n="+n+" ok="+ok+" io="+ioTag+" sz="+ioSz+" cam="+(cam?"y":"n")+" "+vw+"x"+vh+" vh="+(vh|0)+" dir="+dir+" flip="+FLIP);}catch(e){}
  }
}catch(e){}};

var M=G.__ESPSHM__||{};
M.ok=!0;
M.cfg=CFG;
M.layout={hdr:HDR_SZ,ent0:ENT0,entSize:ENT_SZ,magic:MAGIC,ver:VER};
M.offsets={id:O_ID,head:O_HEAD,chest:O_CHEST,pelvis:O_PELVIS,feet:O_FEET,vel:O_VEL,
  hp:O_HP,maxHp:O_MAXHP,dist:O_DIST,flags:O_FLAGS,name:O_NAME,hasBones:O_HB,screenSpace:O_SS,
  sHead:O_SH,sChest:O_SC,sFeet:O_SF,sLead:O_SL,leadM:O_LM,dropM:O_DM,tFly:O_FT,
  camp:O_CAMP,aiLevel:O_AI,liveState:O_LS,
  count:OFF_COUNT,mvp:VO_MVP,camPos:VO_CAMPOS,fov:VO_FOV,vw:VO_W,vh:VO_H};
G.__ESPSHM__=M;
return M;
})();
