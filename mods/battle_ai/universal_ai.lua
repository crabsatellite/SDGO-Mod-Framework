-- Capability-driven controller. The native adapter supplies the current form's
-- weapon records and target observations; no VSAI family or machine-ID policy.
assert(UNIT_MOVE_EVENT_MOVE==2 and UNIT_MOVE_EVENT_DASH==3,'native locomotion enum mismatch')
local D={GenericMode=1,Difficulty=1,Enabled=1,AutoTransform=1,DecisionMs=0,ReactionMs=0,TargetHoldMs=0,
  FarTargetSlack=900,FarTargetRatioPercent=200,WeaponHoldMs=0,AttackIntervalMs=0,
  BurstMs=0,BurstPauseMs=0,AirStrafe=1,StrafeSwitchMs=450,StrafeDistance=400,
  JumpMs=600,JumpCooldownMs=1500,BoostReservePercent=20,ThreatMemoryMs=1600,
  StuckMs=1400,ProgressDistance=30,ArrivalDistance=100,RecoveryMs=650,
  RecoveryCooldownMs=1000,RecoveryDistance=350,RepathMs=700,TargetMemoryMs=2500,PatrolMs=1600,LogDecisions=0,
  CoverChance=90,CoverSearchMs=1600,CoverTimeoutMs=2200,OrbwalkStep=120,OrbwalkMs=180,FlankMs=750}
local C={}
for k,v in pairs(D) do C[k]=v end
for k,v in pairs(config or SDGO_BattleAI_TestConfig or {}) do C[k]=v end
-- Old player configuration must not reintroduce an artificial combat pause.
if C.Difficulty>=1 then
  C.GenericMode=1;C.AttackIntervalMs=0
end
local U=SDGO_BattleAI_Universal or {states={},faults=0}
local function Flag(v) return v~=nil and v~=false and v~=0 end
local function ChangeWeapon(npc,slot)
  if SDGO_BattleAI_RequestWeapon then return SDGO_BattleAI_RequestWeapon(slot) end
  return npc:ChangeWeapon(slot)
end
local function ReleaseHeldIntent()
  if SDGO_BattleAI_ReleaseHeldIntent then return SDGO_BattleAI_ReleaseHeldIntent() end
  return 0
end
local function SafeSetFire(npc,o,on)
  if not on then
    if Flag(o.specialBusy) or Flag(o.rangedBusy) then return ReleaseHeldIntent() end
    -- A generic stop must not clear a valid melee combo. Explicit player-style
    -- melee cancels use their own gate below; all other melee actions finish.
    if Flag(o.meleeBusy) then return 0 end
  end
  return npc:SetAttackOnOff(on)
end
local function Min(a,b) if a<b then return a else return b end end
local function Max(a,b) if a>b then return a else return b end end
local function Distance2(a,b)
  local x,y,z=a.x-b.x,a.y-b.y,a.z-b.z
  return x*x+y*y+z*z
end
local function Log(s,reason,now)
  if C.LogDecisions==1 and s.reason~=reason then
    SDGO_BattleAI_Log(string.format('generic NPC %d: %s',s.id,reason))
  end
  s.reason=reason
end
local function DirectMove(npc,o,dir,distance,mode,recovery)
  if Flag(o.defenceMoving) and not recovery then return false end
  -- A refused step still leaves unmet positioning demand. Otherwise the
  -- watchdog resets its motion clock every frame while a cornered bot waits.
  local state=U.states[o.id]
  if state then state.movementIntent=true;state.movementIntentAt=SDGO_BattleAI_Now() end
  local requested=dir
  if SDGO_BattleAI_DirectIntent(requested,mode,2)~=1 then return false end
  if SDGO_BattleAI_DirectIntent(requested,mode,0)==1 then return true end
  if SDGO_BattleAI_SafeStep then
    dir,distance=SDGO_BattleAI_SafeStep(
      dir,distance,UNIT_MOVE_DIR_FORWARD,UNIT_MOVE_DIR_BACKWARD,
      UNIT_MOVE_DIR_LEFT,UNIT_MOVE_DIR_RIGHT)
    if not dir or dir<0 or not distance or distance<=0 then
      SDGO_BattleAI_DirectIntent(requested,mode,-1)
      return false
    end
  end
  -- A direct step owns the motor now. Otherwise Think's trailing FollowPath
  -- can immediately overwrite this command with an old pursuit waypoint.
  SDGO_BattleAI_CancelRoute(1)
  npc:SetMoveTargetPos_Dir(dir,distance,dir,false)
  npc:SetMoveType(mode)
  return SDGO_BattleAI_DirectIntent(requested,mode,1)==1
end
local function State(o,now)
  local s=U.states[o.id]
  if not s or s.generation~=o.generation or now-s.seen>6000 then
    s={id=o.id,generation=o.generation,seen=now,decisionAt=0,targetAt=0,weaponAt=0,
      turnAt=0,hopAt=0,repathAt=0,progressAt=now,anchor=o.pos,motionPos=o.pos,motionAt=now,
      attempt=0,recoveryAt=0,memory={},blocked={},patrolAt=0}
    U.states[o.id]=s
  end
  s.seen=now
  if not s.blocked then s.blocked={} end
  if s.unitId~=o.unitId or s.alternate~=o.alternate or s.formRevision~=o.formRevision then
    if s.unitId~=nil then SDGO_BattleAI_CancelRoute(1);s.formHoldUntil=now+1000 end
    s.unitId=o.unitId;s.alternate=o.alternate;s.slot=nil;s.weaponAt=0;s.readyAt=nil
    s.formRevision=o.formRevision;s.requestedSlot=nil;s.stationaryBoostAnchor=nil
    s.weave=nil;s.cover=nil;s.orbUntil=nil
    s.meleeAdvanceTarget=nil;s.meleeAdvanceAt=0;s.meleeAdvanceBlocked=nil
    s.recoverUntil=nil;s.recovering=nil;s.routeRecoveryUntil=nil
    s.exploring=nil;s.flanking=false;s.movementIntent=false;s.repathAt=0
  end
  if not U.sweepAt or now>=U.sweepAt then
    U.sweepAt=now+2000
    for id,old in pairs(U.states) do
      if now-old.seen>6000 then U.states[id]=nil
      else for target,memory in pairs(old.memory) do
        if now-memory.seen>C.TargetMemoryMs+1000 then old.memory[target]=nil end
      end end
    end
  end
  return s
end
local function Ordinary(w)
  if not w or w.slot==nil or w.slot<0 or w.slot>=3 or w.attackStyle==2 then return false end
  if w.kind=='melee' or w.kind=='hitscan' or w.kind=='projectile' or w.kind=='missile' then return true end
  -- Some ordinary scripted weapons have native range/initializer evidence but
  -- no known projectile family. The adapter marks only those records opaque;
  -- an unknown kind without that affirmative receipt remains fail-closed.
  return w.kind=='opaque' and Flag(w.ordinary)
end
local function Usable(w)
  return Ordinary(w) and Flag(w.valid) and w.range and w.range>0 and
    (Flag(w.ammoReady) or Flag(w.active))
end
local function TargetDistance(w,t)
  if w and w.kind=='melee' and t.meleeDistance~=nil then return t.meleeDistance end
  if w and w.kind~='melee' and t.rangedDistance~=nil then return t.rangedDistance end
  return t.distance
end
local function TargetVisible(w,t)
  if w and w.kind=='melee' and t.meleeVisible~=nil then return Flag(t.meleeVisible) end
  if w and w.kind~='melee' and t.rangedVisible~=nil then return Flag(t.rangedVisible) end
  return Flag(t.visible)
end
local function AnyVisible(t)
  if Flag(t.perceptionVisible) then return true end
  if t.rangedVisible~=nil or t.meleeVisible~=nil then
    return Flag(t.rangedVisible) or Flag(t.meleeVisible)
  end
  return Flag(t.visible)
end
local function ContactVisible(t)
  return AnyVisible(t) or Flag(t.teamVisible)
end
local function RoleDistance(o,t)
  if Flag(o.meleeRole) and t.meleeDistance~=nil then return t.meleeDistance end
  if not Flag(o.meleeRole) and t.rangedDistance~=nil then return t.rangedDistance end
  return t.distance
end
local function DamageProtected(t)
  return t and Flag(t.damageProtected)
end
local function TargetAlive(t)
  assert(t and t.alive~=nil,'native target lifecycle unavailable')
  return Flag(t.alive)
end
local function InRange(w,t)
  local distance=TargetDistance(w,t)
  if not Usable(w) or not distance or DamageProtected(t) or distance>w.range then return false end
  return TargetVisible(w,t) and Flag(t.attackable)
end
local function AnyInRange(o,t)
  for _,w in pairs(o.weapons) do if InRange(w,t) then return true end end
  return false
end
local function RangedResponse(o)
  if Flag(o.meleePressure) or Flag(o.defenceMoving) then
    for _,w in pairs(o.weapons or {}) do
      if Usable(w) and w.kind~='melee' then return true end
    end
  end
  return false
end
local function ResponseInRange(o,t)
  if not RangedResponse(o) then return AnyInRange(o,t) end
  for _,w in pairs(o.weapons) do
    if w.kind~='melee' and InRange(w,t) then return true end
  end
  return false
end
local function Candidate(o,s,t,now)
  return t.id~=s.id and TargetAlive(t) and not DamageProtected(t) and Flag(t.enemy) and
    (Flag(t.attackable) or Flag(t.teamVisible))
end
local function PickTarget(o,s,now)
  o.rangedResponse=RangedResponse(o)
  local current=nil
  for _,t in pairs(o.targets) do
    if t.id==s.target and Candidate(o,s,t,now) then current=t end
    if Candidate(o,s,t,now) and ContactVisible(t) then
      local copy=s.memory[t.id] or {}
      for key in pairs(copy) do if key~='seen' and t[key]==nil then copy[key]=nil end end
      for key,value in pairs(t) do copy[key]=value end
      copy.seen=now;s.memory[t.id]=copy
    elseif not TargetAlive(t) or DamageProtected(t) then s.memory[t.id]=nil end
  end
  local currentRange=current and ResponseInRange(o,current)
  local best,near=nil,nil
  for _,t in pairs(o.targets) do
    if Candidate(o,s,t,now) and ContactVisible(t) then
      local distance=RoleDistance(o,t)
      local nearDistance=near and RoleDistance(o,near)
      if not near or distance<nearDistance or (distance==nearDistance and t.id<near.id) then near=t end
    end
  end
  for _,t in pairs(o.targets) do
    if Candidate(o,s,t,now) and ContactVisible(t) then
      local distance=RoleDistance(o,t)
      local reference=Flag(o.meleeRole) and near or (currentRange and current)
      local referenceDistance=reference and RoleDistance(o,reference)
      local tooFar=reference and distance>referenceDistance+C.FarTargetSlack and
        distance*100>referenceDistance*C.FarTargetRatioPercent
      if ResponseInRange(o,t) and not tooFar and
         (not best or t.hp<best.hp or (t.hp==best.hp and
          (distance<RoleDistance(o,best) or (distance==RoleDistance(o,best) and t.id<best.id)))) then best=t end
    end
  end
  if Flag(o.meleeRole) then best=best or near or (currentRange and current)
  else best=best or (currentRange and current) or near end
  if not best and s.target then
    local last=s.memory[s.target]
    if last and now-last.seen<C.TargetMemoryMs and Candidate(o,s,last,now) then
      local copy={}
      for key,value in pairs(last) do copy[key]=value end
      copy.visible=0;copy.perceptionVisible=0;copy.rangedVisible=0;copy.meleeVisible=0;copy.teamVisible=0
      copy.remembered=true;best=copy
    end
  end
  return best
end
local function WeaponScore(w,t,threat)
  if not Usable(w) then return -1 end
  local cycle=w.cycleMs
  if not cycle or cycle<=0 then cycle=300 else cycle=Max(cycle,50) end
  local score=Max(w.damage or 1,1)*1000/cycle
  local distance=TargetDistance(w,t)
  if distance>w.range then score=score*0.15*w.range/Max(distance,1) end
  if w.kind=='melee' then
    if distance<=w.range*0.9 then score=score*1.2 else score=score*0.6 end
  elseif distance<w.range*0.08 then score=score*0.6 end
  if threat and Flag(w.movingShot) then score=score*1.15 end
  if Flag(w.ready)==false then score=score*0.85 end
  return score
end
local function PickWeapon(o,s,t,threat,now,forceKind)
  local best,current=nil,nil
  local score,tier=-1,-1
  for _,w in pairs(o.weapons) do
    local allowed=Ordinary(w) and (not forceKind or
      (forceKind=='melee' and w.kind=='melee') or
      (forceKind=='ranged' and w.kind~='melee'))
    local value=allowed and WeaponScore(w,t,threat) or -1
    if allowed and w.slot==o.selectedSlot and Usable(w) then current=w end
    local rank=InRange(w,t) and (Flag(w.ready) and 2 or 1) or 0
    -- A mobile gun that can fire now keeps output during a native escape.
    -- A stronger stationary gun still obeys its own animation/motor gate.
    if Flag(o.defenceMoving) then rank=rank*2+(rank==2 and w.kind~='melee' and Flag(w.movingShot) and 1 or 0) end
    if value>=0 and (rank>tier or (rank==tier and
       (value>score or (value==score and best and w.slot<best.slot)))) then
      best=w;score=value;tier=rank
    end
  end
  if score<0 then return nil end
  -- Do not pay a native switch/animation cost for a tie or to replace a shot
  -- that can already finish the current target. Cooldown/empty/range changes
  -- still select a newly actionable weapon immediately.
  if current and InRange(current,t) and Flag(current.ready) and not forceKind and
     ((current.damage or 0)>=t.hp or WeaponScore(current,t,threat)>=score) then return current end
  -- With no fireable weapon, do not churn between cooling-down slots. The
  -- first genuinely ready option always wins on the next native observation.
  local readyTier=Flag(o.defenceMoving) and 4 or 2
  if tier<readyTier and current and InRange(current,t) then return current end
  return best
end
local function Chance(percent)
  return SDGO_BattleAI_Chance and SDGO_BattleAI_Chance(percent)==1
end
local function FormWeapons(weapons,t)
  local view={ammo=false,ranged=false,rangedAmmo=false,range=0,melee=0,engage=false,fire=false,finisher=false,meleeValue=0,rangedValue=0}
  for _,w in pairs(weapons or {}) do
    if Ordinary(w) and Flag(w.valid) then
      if w.kind~='melee' then view.ranged=true end
      if Usable(w) then
        view.ammo=true;view.range=Max(view.range,w.range)
        if w.kind=='melee' then view.melee=Max(view.melee,w.range)
        else view.rangedAmmo=true end
        if InRange(w,t) then
          view.engage=true
          if Flag(w.ready) then
            view.fire=true
            if (w.damage or 0)>=t.hp then view.finisher=true end
            local value=Max(w.damage or 1,1)*1000/Max(w.cycleMs or 300,50)
            if w.kind=='melee' then view.meleeValue=Max(view.meleeValue,value)
            else view.rangedValue=Max(view.rangedValue,value) end
          end
        end
      end
    end
  end
  return view
end
local function ChangeForm(o,s,t,now)
  local other=o.otherForm
  if C.AutoTransform~=1 or not t or t.remembered or not other or not Flag(other.allowed) or
     Flag(o.hurt) or Flag(o.meleeBusy) or Flag(o.rangedBusy) or Flag(o.specialBusy) or
     Flag(o.forcedMoving) or Flag(o.transportMotorReserved) or now<(s.formHoldUntil or 0) or
     now<(s.formAttemptAt or 0) then return false end
  local current=FormWeapons(o.weapons,t)
  local nextForm=FormWeapons(other.weapons,t)
  local empty=not current.ammo and nextForm.ammo
  local usableRange=not current.engage and nextForm.engage
  local rangedDry=current.ranged and not current.rangedAmmo and nextForm.rangedAmmo
  local distantMelee=not current.rangedAmmo and current.melee>0 and
    (t.meleeDistance or t.distance)>current.melee*1.5 and nextForm.rangedAmmo and nextForm.range>current.range+200
  local closeMelee=nextForm.melee>0 and (t.meleeDistance or t.distance)<=nextForm.melee*0.9 and
    nextForm.meleeValue>current.rangedValue*1.5 and nextForm.meleeValue>current.meleeValue
  if not empty and not rangedDry and current.finisher then return false end
  if not (empty or usableRange or rangedDry or distantMelee or closeMelee) then return false end
  s.formAttemptAt=now+500
  assert(SDGO_BattleAI_RequestTransform,'native transform request unavailable')
  local queued=SDGO_BattleAI_RequestTransform()==1
  if queued then
    s.weave=nil;s.cover=nil;s.meleeAdvanceTarget=nil;s.recoverUntil=nil;s.recovering=nil
    s.movementIntent=false;s.repathAt=0
    Log(s,empty and 'transform: current ammunition unavailable' or 'transform: other native form fits engagement',now)
  end
  return queued
end
local function MeleeGate(value,cancelTracking)
  if SDGO_BattleAI_MeleeGate then
    SDGO_BattleAI_MeleeGate(value and 1 or 0,cancelTracking and 1 or 0)
  end
end
local function Support(o,t,now)
  for _,a in pairs(o.allies or {}) do
    if a.rangedTarget==t.id and a.rangedAt and now-a.rangedAt>=0 and now-a.rangedAt<=1100 and
       a.pos and t.groundPos and Distance2(a.pos,t.groundPos)<=2500*2500 then return true end
  end
  return false
end
local function Weave(npc,o,s,t,now)
  if C.Difficulty<2 then
    s.weave=nil;MeleeGate(RangedResponse(o))
    return RangedResponse(o) and 'ranged' or nil,false
  end
  local function HasRanged()
    for _,w in pairs(o.weapons) do
      if Usable(w) and w.kind~='melee' and InRange(w,t) then return true end
    end
    return false
  end
  local q=s.weave
  if q and (q.target~=t.id or now-q.started>6500 or not AnyVisible(t)) then s.weave=nil;q=nil end
  if not q and Flag(o.meleeSwinging) and Flag(o.comboKnown) and o.comboMax>=2 and
     o.comboIndex==o.comboMax-2 and o.contactTarget==t.id and now-(o.contactAt or 0)<=650 and
     o.contactCombo==o.comboIndex and o.contactAction==o.actionHandle and
     Flag(t.hurt) and Support(o,t,now) then
    if HasRanged() then
      q={stage='release',target=t.id,started=now,shots=o.shots or 0,goal=Chance(50) and 1 or 2}
      s.weave=q
    end
  end
  MeleeGate(RangedResponse(o) or (q and q.stage=='release'))
  if not q then return RangedResponse(o) and 'ranged' or nil,false end
  if q.stage=='release' then
    npc:SetAttackOnOff(false)
    if Flag(o.meleeBusy) then return nil,true end
    q.shots=o.shots or 0;s.readyAt=nil;s.weaponAt=0
    if HasRanged() then
      q.stage='shoot'
      DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,120,UNIT_MOVE_EVENT_MOVE)
    else q.stage='return' end
    MeleeGate(RangedResponse(o))
  end
  if q.stage=='shoot' then
    local fired=(o.shots or 0)-q.shots
    if fired<q.goal and HasRanged() then return 'ranged',false end
    q.stage='return';s.readyAt=nil;s.weaponAt=0
  end
  if q.stage=='return' then
    if RangedResponse(o) then return 'ranged',false end
    if Flag(o.meleeTracking) or Flag(o.meleeSwinging) then q.stage='finish' end
    return 'melee',false
  end
  if q.stage=='finish' then
    if Flag(o.meleeBusy) then return 'melee',false end
    s.weave=nil
  end
  return nil,false
end
local function RoutePoint(t,p,allowRanged)
  return SDGO_BattleAI_Route(t.id,0,p.x,p.y,p.z,allowRanged and 1 or 0)==1
end
local function Flank(npc,o,s,w,t,now)
  if C.Difficulty<2 then return false end
  local function Release()
    if s.flanking and Flag(o.routeActive) then SDGO_BattleAI_CancelRoute() end
    s.flanking=false
  end
  local mayMove=not Flag(o.rangedBusy) or Flag(w.movingShot) or Flag(o.moveCancelOpen)
  if w.kind=='melee' or not t.forward or not t.groundPos or not TargetVisible(w,t) or not mayMove then
    Release();return false
  end
  local caught=false
  for _,a in pairs(o.allies or {}) do
    if a.contactTarget==t.id and now-(a.contactAt or 0)<=650 and Flag(t.hurt) and
       (Flag(a.meleeSwinging) or Flag(a.meleeTracking)) then caught=true end
  end
  if not caught then Release();return false end
  local f=t.forward;local length=f.x*f.x+f.z*f.z
  if length<0.25 or length>4 then Release();return false end
  -- Facing is a native unit vector. Keep the rear point within rifle range.
  local back=Min(350,w.range*0.25)
  local p={x=t.groundPos.x-f.x*back,y=t.groundPos.y,z=t.groundPos.z-f.z*back}
  if Distance2(o.pos,p)<100*100 then Release();return false end
  local lost=s.flanking and not Flag(o.routeActive)
  if lost or (not s.flanking and now>=(s.flankAt or 0)) then
    s.flankAt=now+C.FlankMs;s.flanking=RoutePoint(t,p,true)
    if s.flanking then Log(s,'rear flank with ranged intent',now);return true end
  end
  if s.flanking and Flag(o.routeActive) then Log(s,'rear flank with ranged intent',now);return true end
  s.flanking=false;return false
end
local function CoverStep(npc,o,s,w,t,now)
  if C.Difficulty<2 then s.cover=nil;s.coverSearching=nil;return false,false end
  if w.kind=='melee' or not t.pos or not SDGO_BattleAI_Cover then s.cover=nil;s.coverSearching=nil;return false,false end
  if s.coverSearching and s.coverSearching~=t.id then s.coverSearching=nil;s.coverAt=0 end
  local q=s.cover
  if q and (q.target~=t.id or now-q.started>C.CoverTimeoutMs or Flag(o.meleePressure) or
     (q.stage=='peek' and not TargetVisible(w,t))) then
    s.cover=nil;q=nil;s.coverSearching=nil;SDGO_BattleAI_CancelRoute()
  end
  if not q and not TargetVisible(w,t) then s.coverSearching=nil;return false,false end
  local mayMove=not Flag(o.rangedBusy) or Flag(w.movingShot) or Flag(o.moveCancelOpen)
  if not q and not mayMove then return false,false end
  if not q and now>=(s.coverAt or 0) then
    s.coverAt=now+C.CoverSearchMs
    if s.coverSearching==t.id or Chance(C.CoverChance) then
      local p=SDGO_BattleAI_Cover(t.id,t.pos.x,t.pos.y,t.pos.z,s.peekLeft and -1 or 1)
      if p and Flag(p.pending) then
        s.coverSearching=t.id;s.coverAt=now
        return false,false
      end
      s.coverSearching=nil
      if p and Flag(p.valid) and p.hide and p.peek then
        q={target=t.id,hide=p.hide,peek=p.peek,stage='peek',started=now,shots=o.shots or 0,retryAt=now+300}
        s.cover=q
        s.peekLeft=not s.peekLeft
        if not RoutePoint(t,q.peek,true) then s.cover=nil;q=nil end
      end
    end
  end
  if not q then return false,false end
  if q.stage=='peek' and mayMove and (now>=(q.retryAt or now+1) or
     (not Flag(o.moving) and not Flag(o.routeActive))) and Distance2(o.pos,q.peek)>25*25 then
    q.retryAt=now+300
    if not RoutePoint(t,q.peek,true) then s.cover=nil;return false,false end
  end
  if (o.shots or 0)>q.shots then
    if q.stage=='peek' then
      q.stage='tuck';SDGO_BattleAI_CancelRoute()
      q.routeAt=now
    end
  end
  if q.stage=='tuck' then
    -- Request only legal native movement cancellation. Never set B38/cooldown.
    if not Flag(o.rangedBusy) or Flag(w.movingShot) or Flag(o.moveCancelOpen) then
      if not q.routeAt and not Flag(o.routeActive) and not Flag(o.moving) and Distance2(o.pos,q.hide)>=25*25 then q.routeAt=now end
      if q.routeAt and now>=q.routeAt then
        if RoutePoint(t,q.hide,true) then q.routeAt=nil
        else
          -- Route failure already stopped the native motor. Abandon this
          -- cover opportunity so ordinary combat can take over this frame
          -- instead of stopping its movement again on every observation.
          s.cover=nil;q=nil
          return false,false
        end
      end
      if Distance2(o.pos,q.hide)<25*25 then
        s.cover=nil;s.coverAt=0
        return false,false
      end
    end
    Log(s,'cover: retract after primary fire',now);return true,not mayMove
  end
  Log(s,'cover: smallest fireable edge',now)
  return true,TargetVisible(w,t) and TargetDistance(w,t)<=w.range
end
local function Orbwalk(npc,o,s,w,t,now)
  if C.Difficulty<2 then return false,false end
  if w.kind=='melee' or not TargetVisible(w,t) or TargetDistance(w,t)>w.range or
     (not Flag(t.meleeApproaching) and not Flag(t.meleeTracking) and not Flag(t.meleeSwinging) and not Flag(t.hurt) and not Support(o,t,now)) then return false,false end
  local count=o.shots or 0
  if s.orbShots==nil then s.orbShots=count end
  if count>s.orbShots then
    s.orbShots=count;s.orbPending=true
  end
  if s.orbPending and (not Flag(o.rangedBusy) or Flag(o.moveCancelOpen)) and now>=(s.orbUntil or 0) then
    s.orbPending=false;s.orbUntil=now+C.OrbwalkMs;s.orbLeft=not s.orbLeft
    local dir=s.orbLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    DirectMove(npc,o,dir,C.OrbwalkStep,
      (o.boostFraction or 0)*100>C.BoostReservePercent and UNIT_MOVE_EVENT_DASH or UNIT_MOVE_EVENT_MOVE)
  end
  if now<(s.orbUntil or 0) then
    -- A native ready weapon ends a stationary-only step immediately. The step
    -- duration is a movement goal, never an extra firing cooldown.
    if (not Flag(w.movingShot) and not Flag(o.rangedBusy) and Flag(w.ready)) or
       (not Flag(o.moving) and o.goal and Distance2(o.pos,o.goal)<=C.ArrivalDistance*C.ArrivalDistance) then
      s.orbUntil=nil
      return false,false
    end
    Log(s,'orbwalk: legal step after primary fire',now)
    return true,not Flag(w.movingShot)
  end
  return false,false
end
local function RouteMotion(o)
  local mode=(o.boostFraction or 0)*100>C.BoostReservePercent and UNIT_MOVE_EVENT_DASH or UNIT_MOVE_EVENT_MOVE
  SDGO_BattleAI_RouteMotion(mode)
end
local function Move(npc,o,s,target,now,force)
  RouteMotion(o)
  if now<s.repathAt and not force then return false end
  s.repathAt=now+C.RepathMs
  local status
  if target.remembered and target.groundPos then
    local p=target.groundPos
    status=SDGO_BattleAI_Route(target.id,force and 1 or 0,p.x,p.y,p.z)
  else status=SDGO_BattleAI_Route(target.id,force and 1 or 0) end
  if status==0 then
    s.blocked[target.id]=now+C.RepathMs
    if target.remembered then
      -- That last-seen point is no longer reachable. A fresh visible contact
      -- can be selected immediately; do not wait out its memory lifetime by
      -- repeatedly forcing the same failed route from a corner.
      s.memory[target.id]=nil
    end
    -- Keep unmet pursuit visible to recovery. The geometry planner already
    -- validates direct edges; a failed route must not turn into another
    -- unplanned approach request.
    s.movementIntent=true;s.movementIntentAt=now
    Log(s,'route unavailable: retry throttled',now)
    return false
  end
  s.blocked[target.id]=nil
  s.movementIntent=true;s.movementIntentAt=now
  return true
end
local function MeleeReach(o)
  local reach=0
  for _,w in pairs(o.weapons or {}) do
    if Ordinary(w) and Flag(w.valid) and w.kind=='melee' and w.range and w.range>reach then reach=w.range end
  end
  return reach
end
local function MeleeOpportunity(o,t)
  if not Flag(o.meleeRole) or Flag(o.meleePressure) then return false end
  for _,w in pairs(o.weapons or {}) do
    if w.kind=='melee' and InRange(w,t) then return true end
  end
  return false
end
local function MeleeApproach(npc,o,s,w,t,now)
  if not Flag(o.meleeRole) or Flag(o.meleePressure) or t.remembered or
     Flag(o.defenceMoving) or Flag(o.meleeBusy) or Flag(o.specialBusy) or not AnyVisible(t) then
    s.meleeAdvanceTarget=nil
    return false
  end
  local reach=MeleeReach(o)
  local distance=t.meleeDistance or t.distance
  if reach<=0 or not distance or distance<=Max(100,reach*0.9) then
    if s.meleeAdvanceTarget==t.id and Flag(o.routeActive) then SDGO_BattleAI_CancelRoute() end
    s.meleeAdvanceTarget=nil
    return false
  end
  -- Direct bounded closing follows the visible combat/aim point. A large
  -- target's navigation root can be outside the playable map even while its
  -- exposed body is in legal melee space; full route planning may still use
  -- groundPos elsewhere.
  local point=t.pos or t.typedAimPos or t.typedAimpos or t.groundPos
  if not point then return false end
  if s.meleeAdvanceTarget~=t.id then
    if Flag(o.routeActive) then SDGO_BattleAI_CancelRoute() end
    s.meleeAdvanceTarget=t.id;s.meleeAdvanceAt=0;s.meleeAdvanceBlocked=nil
  end
  if Flag(o.rangedBusy) and not Flag(w.movingShot) and not Flag(o.moveCancelOpen) then
    Log(s,'melee role: wait for legal ranged movement window',now)
    return true
  end
  if Flag(o.routeActive) then
    s.movementIntent=true;s.movementIntentAt=now
    Log(s,'melee role: continue bounded advance',now)
    return true
  end
  if now<(s.meleeAdvanceAt or 0) then return not s.meleeAdvanceBlocked end
  s.meleeAdvanceAt=now+C.RepathMs
  local allowRanged=w and w.kind~='melee' and Flag(w.movingShot)
  local step=Min(400,Max(100,distance-reach*0.75))
  RouteMotion(o)
  local advanced=SDGO_BattleAI_AdvancePoint and
    SDGO_BattleAI_AdvancePoint(point.x,point.y,point.z,step,allowRanged and 1 or 0)==1
  -- A blocked closing step still needs movement. Keep that demand visible to
  -- the same progress watchdog used by ordinary route pursuit.
  s.movementIntent=true;s.movementIntentAt=now
  if advanced then
    s.meleeAdvanceBlocked=nil
    Log(s,'melee role: close while keeping legal output',now)
    return true
  end
  s.meleeAdvanceBlocked=true
  Log(s,'melee role: direct advance blocked; plan a map route',now)
  return false
end
local function RecoveryMode(active)
  if SDGO_BattleAI_RecoveryMode then SDGO_BattleAI_RecoveryMode(active and 1 or 0) end
end
local function Recover(npc,o,s,t,now)
  local motion=s.motionPos or o.pos
  local motionX,motionZ=o.pos.x-motion.x,o.pos.z-motion.z
  s.motionPos=o.pos
  local verticalAnchor=s.verticalAnchor or o.pos
  local verticalX,verticalZ=o.pos.x-verticalAnchor.x,o.pos.z-verticalAnchor.z
  local verticalProgress=false
  if not s.verticalLowY or verticalX*verticalX+verticalZ*verticalZ>=C.ProgressDistance*C.ProgressDistance then
    s.verticalAnchor=o.pos;s.verticalLowY=o.pos.y;s.verticalHighY=o.pos.y
  elseif not Flag(o.hurt) and not Flag(o.meleeBusy) and not Flag(o.specialBusy) then
    if o.pos.y<s.verticalLowY-2 then s.verticalLowY=o.pos.y;verticalProgress=true
    elseif o.pos.y>s.verticalHighY+2 then s.verticalHighY=o.pos.y;verticalProgress=true end
  end
  if motionX*motionX+motionZ*motionZ>0.01 or verticalProgress then
    s.motionAt=now;s.routeRecoveryUntil=nil
  end
  if verticalProgress then s.anchor=o.pos;s.progressAt=now;s.attempt=0 end
  if Flag(o.hurt) or Flag(o.meleeBusy) or Flag(o.specialBusy) then
    s.recoverUntil=nil;s.recovering=nil;s.routeRecoveryUntil=nil;s.anchor=o.pos;s.progressAt=now;RecoveryMode(false)
    s.motionPos=o.pos;s.motionAt=now
    return false
  end
  if o.movementState==72 or o.movementPending==72 then
    -- Boost startup owns a native animation before displacement is possible.
    -- Do not let the shorter no-motion watchdog cancel that admitted motor.
    -- The independent activity observer still measures actual inactivity.
    s.recoverUntil=nil;s.recovering=nil;s.routeRecoveryUntil=nil
    s.anchor=o.pos;s.progressAt=now;s.motionPos=o.pos;s.motionAt=now
    RecoveryMode(false);return false
  end
  if t and Flag(t.mapObstacle) then
    for _,weapon in pairs(o.weapons) do
      if Usable(weapon) and InRange(weapon,t) and Flag(weapon.ready) then
        -- A closed route can require an ordinary shot or melee swing first.
        -- Give the attack decision its motor admission instead of starting a
        -- recovery strafe that suppresses a stationary-only breaching gun.
        s.recoverUntil=nil;s.recovering=nil;s.routeRecoveryUntil=nil
        s.movementIntent=false;s.motionPos=o.pos;s.motionAt=now
        RecoveryMode(false);return false
      end
    end
  end
  -- A stationary-only shot owns the motor until its real cancellation window.
  local mayMove=not Flag(o.rangedBusy) or Flag(o.moveCancelOpen)
  if not mayMove then
    for _,w in pairs(o.weapons) do if w.slot==o.selectedSlot and Flag(w.movingShot) then mayMove=true end end
  end
  if not mayMove then s.motionPos=o.pos;s.motionAt=now end
  -- A native pad request retains its bridge's bounded notification deadline. Keep the admitted
  -- entrance goal while the original bridge waits for its real notification;
  -- do not reset progress clocks or count this intent as physical activity.
  if Flag(o.transportWaiting) then return false end
  if s.routeRecoveryUntil then
    if mayMove and now<s.routeRecoveryUntil then
      -- Native movement admission spans more than one frame. Keep the newly
      -- accepted plan alive until it can produce its first physical step;
      -- this is not the side-step lease that blocks native FollowPath.
      RecoveryMode(false);return true
    end
    s.routeRecoveryUntil=nil
  end
  -- Net route progress and a motor that has not moved at all are different.
  -- Keep the configurable route-progress window, but do not wait 1.4 seconds
  -- to react to a stationary free controller. Small real steps count too.
  -- Stationary guns and a committed weave retain their established motor/
  -- attack handoff window. Mobile pursuit can begin its recovery sooner.
  local motionWindow=s.weave and 350 or 150
  for _,weapon in pairs(o.weapons) do
    if weapon.slot==o.selectedSlot and not Flag(weapon.movingShot) then motionWindow=350 end
  end
  local stationary=now-(s.motionAt or now)>=motionWindow
  if s.recoverUntil and s.recovering and now<s.recoverUntil and
     (not stationary or now-(s.recoveryStartedAt or now)<Min(C.RecoveryMs,500)) and
     (Flag(o.moving) or not o.goal or Distance2(o.pos,o.goal)>C.ArrivalDistance*C.ArrivalDistance) then
    if mayMove then
      RecoveryMode(true);npc:SetMoveType(UNIT_MOVE_EVENT_MOVE);npc:SetJumpTime(0)
      if not Flag(o.moving) and s.recoveryDir then
        DirectMove(npc,o,s.recoveryDir,C.RecoveryDistance,UNIT_MOVE_EVENT_MOVE,true)
      end
    end
    return mayMove
  end
  if s.recoverUntil then
    if s.recovering and stationary and
       (Flag(o.moving) or not o.goal or Distance2(o.pos,o.goal)>C.ArrivalDistance*C.ArrivalDistance) then
      -- A validated goal needs time to pass the native stop/start transition.
      -- If it still made no progress, plan a route now rather than reserving
      -- another stationary relative-goal admission interval.
      s.attempt=2
    end
    s.recoverUntil=nil;s.recovering=nil;s.anchor=o.pos;s.progressAt=now;s.repathAt=0
    if not Flag(o.moving) and o.goal and Distance2(o.pos,o.goal)<=C.ArrivalDistance*C.ArrivalDistance then
      s.motionPos=o.pos;s.motionAt=now;stationary=false
    end
  end
  RecoveryMode(false)
  local intended=s.movementIntent and now-(s.movementIntentAt or 0)<=C.StuckMs+C.RepathMs
  local wanted=Flag(o.moving) or Flag(o.routeActive) or Flag(o.defenceMoving) or intended or s.cover or s.flanking
  if not wanted or (not intended and (not o.goal or Distance2(o.pos,o.goal)<=C.ArrivalDistance*C.ArrivalDistance)) then
    s.anchor=o.pos;s.progressAt=now;s.motionPos=o.pos;s.motionAt=now;return false
  end
  local dx,dz=o.pos.x-s.anchor.x,o.pos.z-s.anchor.z
  -- Repeated vertical hopping against a wall is not horizontal path progress.
  if dx*dx+dz*dz>=C.ProgressDistance*C.ProgressDistance then
    s.anchor=o.pos; s.progressAt=now; s.attempt=0; return false
  end
  if not stationary and (now-s.progressAt<C.StuckMs or now<s.recoveryAt) then return false end
  if not mayMove then return false end
  s.attempt=s.attempt+1;s.recoveryStartedAt=now;s.recoveryAt=now+C.RecoveryMs+C.RecoveryCooldownMs
  RecoveryMode(true)
  SDGO_BattleAI_CancelRoute(1)
  -- CancelRoute already releases its owned native motor. A second MoveStop
  -- requeues stopping ahead of the replacement relative goal and can keep
  -- restarting the native stop/start animation while the watchdog retries.
  npc:SetJumpTime(0)
  local started=false
  while s.attempt<=2 do
    local dir=s.attempt==1 and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    s.recoveryDir=dir
    if DirectMove(npc,o,dir,C.RecoveryDistance,UNIT_MOVE_EVENT_MOVE,true) then
      s.recoverUntil=now+C.RecoveryMs;s.recovering=true;started=true
      break
    end
    -- A collision-rejected command never acquired the motor. Try the next
    -- direction now instead of reserving an idle lease for work not started.
    s.attempt=s.attempt+1
  end
  if not started then
    s.recoveryDir=nil
    -- A planned route now owns recovery. Do not reserve another side-step
    -- lease or discard the new route on the next decision.
    RecoveryMode(false)
    local planned=false
    if t then planned=Move(npc,o,s,t,now,true)
    else
      s.exploring=SDGO_BattleAI_ExploreMap()==1;planned=s.exploring
      s.movementIntent=true;s.movementIntentAt=now
    end
    s.routeRecoveryUntil=planned and now+200 or nil
    s.attempt=0;s.anchor=o.pos;s.progressAt=now;s.recoverUntil=nil;s.recovering=nil
  end
  Log(s,'stuck recovery',now)
  return true
end
local function GroundStrafe(npc,o,s,w,now,canFire)
  if C.Difficulty~=1 or not canFire or not Flag(w.movingShot) or
     w.kind=='melee' or w.kind=='other' then return false end
  local turn=now>=(s.groundTurnAt or 0)
  if turn then
    s.groundLeft=not s.groundLeft
    s.groundDir=s.groundLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
  end
  if not s.groundDir then s.groundDir=UNIT_MOVE_DIR_LEFT end
  if turn or not Flag(o.moving) or Flag(o.routeActive) then
    if not DirectMove(npc,o,s.groundDir,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE) then return false end
    if turn then s.groundTurnAt=now+C.StrafeSwitchMs end
  end
  return true
end
local function Evade(npc,o,s,w,t,now,canFire)
  if C.Difficulty<2 then return false end
  if C.AirStrafe~=1 or w.kind=='melee' or now>=(s.threatUntil or 0) then return false end
  -- Stationary-only weapons evade between shots, not by bypassing their motor rules.
  if not Flag(w.movingShot) and canFire then return false end
  local boost=(o.boostFraction or 0)*100>C.BoostReservePercent
  if not boost and s.evadeBoost then
    -- Exhaustion is not rate-limited by the lateral direction timer. Release
    -- held lift/thrust on this observation, even while the motor still moves.
    npc:SetMoveType(UNIT_MOVE_EVENT_MOVE);npc:SetJumpTime(0);s.evadeBoost=false
  end
  local turn=now>=s.turnAt
  if turn or not Flag(o.moving) or Flag(o.routeActive) then
    if turn then s.left=not s.left; s.turnAt=now+C.StrafeSwitchMs end
    local dir=s.left and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
    DirectMove(npc,o,dir,C.StrafeDistance,boost and UNIT_MOVE_EVENT_DASH or UNIT_MOVE_EVENT_MOVE)
    s.evadeBoost=boost
    if boost and now>=s.hopAt and Flag(o.canJump) then
      npc:SetJumpTime(C.JumpMs/1000)
      s.hopAt=now+C.JumpCooldownMs
    end
  end
  Log(s,'ranged duel: air strafe',now)
  return true
end
local function SpecialTarget(o,t)
  local distance=t and (t.rangedDistance or t.distance)
  local visible=false
  if t then
    if t.perceptionVisible~=nil then visible=Flag(t.perceptionVisible)
    elseif t.rangedVisible~=nil then visible=Flag(t.rangedVisible)
    else visible=Flag(t.visible) end
  end
    return t and distance and TargetAlive(t) and not Flag(t.mapObstacle) and not DamageProtected(t) and visible and
    Flag(t.attackable) and distance<=(o.specialRange or 0)
end
local function HasOrdinary(o,slot)
  if not slot or slot<0 or slot>=3 then return false end
  for _,w in pairs(o.weapons or {}) do if Ordinary(w) and w.slot==slot and Flag(w.valid) then return true end end
  return false
end
local function AllyCatch(o,t,now)
  if not Flag(t.hurt) then return false end
  for _,a in pairs(o.allies or {}) do
    if a.contactTarget==t.id and now-(a.contactAt or 0)>=0 and now-(a.contactAt or 0)<=650 and
       (Flag(a.meleeTracking) or Flag(a.meleeSwinging)) then return true end
  end
  return false
end
local function OrdinaryFinisher(o,t)
  for _,w in pairs(o.weapons or {}) do
    if Ordinary(w) and InRange(w,t) and Flag(w.ready) and (w.damage or 0)>=t.hp then return true end
  end
  return false
end
local function KeepBoundGun(npc,o,t)
  local fire=false
  if not Flag(o.hurt) then
    for _,w in pairs(o.weapons or {}) do
      if Ordinary(w) and w.slot==o.selectedSlot and w.kind~='melee' and InRange(w,t) and
         (Flag(w.ready) or Flag(w.active)) then fire=true end
    end
  end
  npc:SetAttackOnOff(fire)
  SDGO_BattleAI_AimTarget(fire and t.id or 0)
end
local function ProfileTick(npc,o,now,deferSpecial)
  if C.Difficulty<1 or not Flag(o.valid) or o.hp<=0 then return false end
  if not U.profiles then U.profiles={} end
  if now>=(U.profileSweepAt or 0) then
    U.profileSweepAt=now+2000
    for id,old in pairs(U.profiles) do if now-(old.seen or 0)>10000 then U.profiles[id]=nil end end
  end
  local p=U.profiles[o.id]
  if not p or p.generation~=o.generation or now-(p.seen or 0)>6000 then
    p={generation=o.generation,seen=now};U.profiles[o.id]=p
  end
  p.seen=now
  local specialControl=not Flag(o.hurt)
  if o.specialControlAllowed~=nil then specialControl=Flag(o.specialControlAllowed) end
  local lockedAttack=Flag(o.meleeBusy) and not Flag(o.hurt)
  local q=p.special
  local function Release()
    p.special=nil
    if SDGO_BattleAI_ProfileLease then SDGO_BattleAI_ProfileLease(-1) end
    return false
  end
  local function ReturnWeapon()
    q.returning=true;q.returnAt=q.returnAt or now
    -- A transform may remove the old slot. Do not freeze the whole controller
    -- waiting for an impossible restoration; normal selection owns the new form.
    if HasOrdinary(o,o.selectedSlot) or not HasOrdinary(o,q.slot) or now-q.returnAt>=250 then return Release() end
    -- A lease always owns either the selected special record or a natively
    -- acknowledged action. Clear only that held request; the stock attack-off
    -- receiver queues Neutral and can cut the action that is handing off.
    ReleaseHeldIntent()
    ChangeWeapon(npc,q.slot)
    return true
  end
  if not q and Flag(o.specialBusy) then
    -- A native/stock/late-observed special can predate this Lua lease. It
    -- still owns its animation, but no stale held input may keep it open.
    ReleaseHeldIntent();p.orphanSpecial=true
    return true
  end
  if p.orphanSpecial then
    -- The native busy predicate is now clear. Give the generic controller
    -- this same decision to keep an ordinary slot or request one.
    p.orphanSpecial=nil
    return false
  end
  if q then
    if Flag(o.specialBusy) then
      -- Admission has already happened. Release the held slot-3 input so a
      -- miss or a surviving SP budget cannot immediately re-open the special
      -- after the native animation; the native FSM still owns completion.
      ReleaseHeldIntent()
      q.active=true;q.returnAt=nil
      return true
    end
    local admitted=Flag(o.specialAcknowledged) and (o.specialAckSequence or 0)>(q.ackSequence or 0)
    -- A begin receipt followed by an actually committed ordinary slot is a
    -- stronger handoff receipt than a possibly lost finish packet. This cannot
    -- be the initial switch delay: admission only occurs after slot 3 ran.
    if admitted and HasOrdinary(o,o.selectedSlot) then return Release() end
    if admitted and (o.specialFinished or 0)<=(q.finished or 0) then
      -- The server begin receipt proves this trigger was accepted. Releasing
      -- held input cannot cancel the owned FSM state and prevents it from
      -- queuing another trigger while that state finishes.
      ReleaseHeldIntent()
      if Flag(o.attackNeutral) then
        return ReturnWeapon()
      end
      -- Current or pending native state still owns the admitted action. Do not
      -- replace that evidence with an arbitrary elapsed-time cutoff.
      q.returnAt=nil
      return true
    end
    if q.returning then return ReturnWeapon() end
    if q.active or (o.specialFinished or 0)>(q.finished or 0) then return ReturnWeapon() end
    -- Selecting slot three is only a request. A neutral FSM with no begin
    -- receipt owns no action, so it cannot reserve the whole controller while
    -- native admission keeps rejecting it. Current AND pending states must be
    -- neutral; tracking, an admitted cast and a queued cast keep their owner.
    if Flag(o.attackNeutral) then q.neutralAt=q.neutralAt or now
    else q.neutralAt=nil end
    if not admitted and q.neutralAt and now-q.neutralAt>=200 then
      p.specialRetryAt=now+1000
      return ReturnWeapon()
    end
    if not specialControl or lockedAttack then
      -- The bridge only clears an owned slot-three held request here. If a
      -- melee action currently owns the FSM and slot three was not committed,
      -- it deliberately does nothing so that melee can finish naturally.
      if Flag(o.specialSelected) or admitted or Flag(o.specialBusy) or Flag(o.rangedBusy) then
        ReleaseHeldIntent()
      end
      return true
    end
    local target=nil
    for _,t in pairs(o.targets or {}) do if t.id==q.target and SpecialTarget(o,t) then target=t end end
    if not target then return ReturnWeapon() end
    if not q.defensive and not admitted and (Flag(o.defenceMoving) or AllyCatch(o,target,now) or OrdinaryFinisher(o,target) or
       (not Flag(target.hurt) and not Flag(target.meleeTracking) and not Flag(target.meleeSwinging))) then return ReturnWeapon() end
    -- A resource rejection/debit without a busy/acknowledged action must not
    -- monopolize the controller.
    if not Flag(o.spReady) then return ReturnWeapon() end
    npc:SetAttackTarget(q.target)
    if not Flag(o.specialSelected) then KeepBoundGun(npc,o,target);ChangeWeapon(npc,3)
    else npc:SetAttackOnOff(true) end
    return true
  end
  if deferSpecial or now<(p.specialRetryAt or 0) or not Flag(o.specialAvailable) or not Flag(o.spReady) or
     not specialControl or lockedAttack or Flag(o.specialBusy) or Flag(o.rangedBusy) then return false end
  local target=nil;local defensiveCast=false
  for _,t in pairs(o.targets or {}) do
    local melee=t.selectedTarget==o.id and (Flag(t.meleeTracking) or Flag(t.meleeSwinging))
    local pressure=t.selectedTarget==o.id and Flag(t.rangedFiring)
    local received=t.id==o.meleeAttacker or (Flag(o.hurt) and t.id==(o.policyTarget or o.nativeTarget))
    local defensive=received or (not AllyCatch(o,t,now) and
      (melee or (pressure and not OrdinaryFinisher(o,t))))
    if SpecialTarget(o,t) and defensive then
      if not target or t.id==o.meleeAttacker or (target.id~=o.meleeAttacker and t.distance<target.distance) then target=t;defensiveCast=true end
    end
  end
  -- Medium reserves SP for pressure/catch defence. Hard can confirm an opening,
  -- but never steal an allied catch, spend SP on a normal one-shot, or stop an
  -- ongoing crossfire escape just because the gauge reached its cap.
  if not target and C.Difficulty>=2 and not Flag(o.defenceMoving) then
    for _,t in pairs(o.targets or {}) do
      if t.id==(o.policyTarget or o.nativeTarget) and SpecialTarget(o,t) and
         (Flag(t.hurt) or Flag(t.meleeTracking) or Flag(t.meleeSwinging)) and
         not AllyCatch(o,t,now) and not OrdinaryFinisher(o,t) then target=t end
    end
  end
  if not target then return false end
  if U.releases then U.releases[o.id]=nil end
  p.special={target=target.id,slot=o.selectedSlot,started=now,neutralAt=Flag(o.attackNeutral) and now or nil,finished=o.specialFinished or 0,
             ackSequence=o.specialAckSequence or 0,defensive=defensiveCast}
  if SDGO_BattleAI_ProfileLease then SDGO_BattleAI_ProfileLease(o.selectedSlot) end
  -- A free native controller may commit the slot in this decision. Bind the
  -- confirmed counter target before its retained firing input can be consumed.
  npc:SetAttackTarget(target.id)
  MeleeGate(false);KeepBoundGun(npc,o,target);ChangeWeapon(npc,3)
  return true
end
local function ProtectedTarget(o,s)
  local held,native,nearest=nil,nil,nil
  for _,t in pairs(o.targets or {}) do
      if TargetAlive(t) and Flag(t.enemy) and ContactVisible(t) and DamageProtected(t) then
      if t.id==s.target then held=t
      elseif t.id==o.nativeTarget or t.id==o.policyTarget then native=t
      elseif not nearest or t.distance<nearest.distance or
             (t.distance==nearest.distance and t.id<nearest.id) then nearest=t end
    end
  end
  return held or native or nearest
end
local function SelectedWeapon(o)
  local fallback=nil
  for _,w in pairs(o.weapons or {}) do
    if Ordinary(w) and Flag(w.valid) and w.range and w.range>0 then
      if w.slot==o.selectedSlot then return w end
      if not fallback or (w.kind~='melee' and fallback.kind=='melee') or w.range>fallback.range then fallback=w end
    end
  end
  return fallback
end
local function PositionForProtected(npc,o,s,t,now)
  if Flag(o.specialBusy) or Flag(o.rangedBusy) then
    ReleaseHeldIntent()
  elseif not Flag(o.meleeBusy) then
    npc:SetAttackOnOff(false)
  end
  SDGO_BattleAI_Cadence(0)
  npc:SetAttackTarget(t.id);SDGO_BattleAI_AimTarget(t.id)
  s.patrolling=false;s.weave=nil;s.cover=nil;s.flanking=false;s.cooldownMoving=false
  npc:SetJumpTime(0)
  local w=SelectedWeapon(o)
  if not w or Flag(o.meleeBusy) or Flag(o.specialBusy) then
    Log(s,'protected target: hold attack until native action ends',now)
    return
  end
  local mayMove=not Flag(o.rangedBusy) or Flag(o.moveCancelOpen) or Flag(w.movingShot)
  if not mayMove then
    Log(s,'protected target: hold legal stationary action',now)
    return
  end
  if Flag(o.defenceMoving) then
    npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
    Log(s,'protected target: retain defensive positioning',now)
    return
  end
  local range=Max(w.range or 0,120)
  local desired=w.kind=='melee' and Max(120,range*0.8) or Max(300,range*0.65)
  local near=Max(90,desired*0.65)
  local far=desired+Max(80,range*0.1)
  local distance=TargetDistance(w,t)
  if distance<near then
    DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,Min(C.StrafeDistance,Max(100,near-distance+80)),UNIT_MOVE_EVENT_MOVE)
    Log(s,'protected target: walk back to engagement range',now)
    return
  end
  local point=t.groundPos or t.pos
  if distance>far and point and o.pos then
    local dx,dz=o.pos.x-point.x,o.pos.z-point.z
    local length=distance
    if dx*dx+dz*dz>1 and length>1 and
       (not Flag(o.routeActive) or now>=(s.protectedRepathAt or 0)) then
      local p={x=point.x+dx*desired/length,y=point.y,z=point.z+dz*desired/length}
      s.protectedRepathAt=now+C.RepathMs
      if RoutePoint(t,p,true) then
        s.movementIntent=true;s.movementIntentAt=now
        npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
        Log(s,'protected target: walk to engagement range',now)
        return
      end
    elseif Flag(o.routeActive) then
      npc:SetMoveType(UNIT_MOVE_EVENT_MOVE)
      Log(s,'protected target: continue engagement route',now)
      return
    end
  end
  if now>=(s.protectedStepAt or 0) or not Flag(o.moving) then
    s.protectedLeft=not s.protectedLeft
    s.protectedStepAt=now+C.StrafeSwitchMs
    DirectMove(npc,o,s.protectedLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT,
      Min(C.StrafeDistance,200),UNIT_MOVE_EVENT_MOVE)
  end
  Log(s,'protected target: walk for the next legal opening',now)
end
local function SelectRouteObstacle(npc,o,s,t,now)
  local obstacle=o.obstacle
  local canBreach=false
  for _,weapon in pairs(o.weapons) do if Usable(weapon) then canBreach=true end end
  if canBreach and obstacle and obstacle.hp>0 and Flag(obstacle.attackable) and AnyVisible(obstacle) and
     (not t or not AnyInRange(o,t)) and not Flag(o.specialBusy) and not Flag(o.forcedMoving) then
    local held=s.target==obstacle.id or o.nativeTarget==obstacle.id
    if Flag(o.meleeTracking) and not Flag(o.meleeSwinging) and not held then
      -- Only the existing native tracking gate can release this chase.
      MeleeGate(true,true)
      Log(s,'release blocked tracking before breaching a route obstacle',now)
      return t,true
    end
    if not Flag(o.meleeBusy) or held then
      t=obstacle;s.weave=nil;s.cover=nil
      -- Releasing the old tracking target is a one-step operation. Clear its
      -- suppression when the blocker becomes the ordinary attack target.
      MeleeGate(false)
    end
  end
  return t,false
end
-- Lua 5.0 limits a closure to32 upvalues. Keep related entry helpers together.
local function BindTarget(npc,o,s,t,now)
  if s.target~=t.id then
    s.target=t.id;s.targetAt=now+C.TargetHoldMs;s.readyAt=nil
    s.repathAt=0;s.meleeAdvanceTarget=nil;s.meleeAdvanceAt=0
    npc:SetAttackTarget(t.id)
    s.cover=nil;s.flanking=false
  elseif o.nativeTarget~=nil and o.nativeTarget~=t.id and Flag(o.attackNeutral) and
         not t.remembered and ContactVisible(t) then
    -- Native weapon/special handoffs can clear the actual target while the
    -- policy keeps the same opponent. Rebind on a free controller instead of
    -- holding fire against an empty native target indefinitely.
    npc:SetAttackTarget(t.id)
  end
end
local function ControllerDiagnostic(o,s,t,now)
  if C.LogDecisions~=1 or now<(s.diagnosticAt or 0) then return end
  local motionAge=now-(s.motionAt or now)
  s.diagnosticAt=now+250
  local profile=U.profiles and U.profiles[o.id]
  local special=profile and profile.special
  local weapon=nil
  for _,candidate in pairs(o.weapons or {}) do if candidate.slot==o.selectedSlot then weapon=candidate end end
  SDGO_BattleAI_Log(string.format('controller unit=%d target=%d distance=%.1f protected=%d own=%d shared=%d melee=%d tracking=%d ranged=%d special=%d neutral=%d recovery=%d remaining=%.0f motion_age=%.0f attempt=%d lease=%d slot=%d requested=%d route=%d moving=%d weapon=%s ready=%d ammo=%d mobile=%d last=%s',
    o.id,t and t.id or 0,t and t.distance or -1,t and DamageProtected(t) and 1 or 0,t and Flag(t.visible) and 1 or 0,t and Flag(t.teamVisible) and 1 or 0,
    Flag(o.meleeBusy) and 1 or 0,Flag(o.meleeTracking) and 1 or 0,Flag(o.rangedBusy) and 1 or 0,
    Flag(o.specialBusy) and 1 or 0,Flag(o.attackNeutral) and 1 or 0,s.recovering and 1 or 0,(s.recoverUntil or now)-now,
    motionAge,s.attempt or 0,special and 1 or 0,o.selectedSlot or -1,s.requestedSlot or -1,
    Flag(o.routeActive) and 1 or 0,Flag(o.moving) and 1 or 0,weapon and weapon.kind or 'none',
    weapon and Flag(weapon.ready) and 1 or 0,weapon and Flag(weapon.ammoReady) and 1 or 0,weapon and Flag(weapon.movingShot) and 1 or 0,tostring(s.reason or 'initial')))
end
local function FinishBoostStart(o,s,w,inRange)
  if not w or w.kind=='melee' or not Flag(w.ready) or not inRange then
    s.stationaryBoostAnchor=nil;return false
  end
  local starting=o.movementState==72 or o.movementPending==72
  local anchor=s.stationaryBoostAnchor
  if starting and not anchor then
    anchor={x=o.pos.x,y=o.pos.y,z=o.pos.z};s.stationaryBoostAnchor=anchor
  end
  if not anchor then return false end
  local dx,dy,dz=o.pos.x-anchor.x,o.pos.y-anchor.y,o.pos.z-anchor.z
  local progressed=dx*dx+dz*dz>0.01 or dy*dy>4
  -- Original 0x48 starts boost without displacement. Braking or issuing a new
  -- strafe before its first step can chain/restart native startup and end.
  -- This admission belongs to mobile scripted guns too. Retain the motor
  -- until a real step, then resume the weapon's ordinary firing/movement path.
  if starting or (o.movementState==92 and not progressed) then return true end
  s.stationaryBoostAnchor=nil;return false
end
local Tactics={profile=ProfileTick,obstacle=SelectRouteObstacle,motion=RouteMotion,
  diagnostic=ControllerDiagnostic,bind=BindTarget,transform=ChangeForm,finishBoost=FinishBoostStart}
local function Tick(npc)
  local o=SDGO_BattleAI_Observe()
  if o and Flag(o.valid) and Flag(o.transforming) then
    -- The original transform Entry/Update/Release owns both controllers and
    -- the bank handoff. Do not turn a transient weapon sample into attack-off
    -- or MoveStop while that native action is committing its new form.
    return 123
  end
  if not o or not Flag(o.valid) or not o.weapons or not o.targets then
    error('native observation unavailable: '..tostring(o and o.failure or 'incomplete observation'))
  end
  local now=SDGO_BattleAI_Now()
  local s=State(o,now)
  if o.transportCompleted~=nil and s.transportCompleted~=o.transportCompleted then
    if s.transportCompleted~=nil then s.repathAt=0;s.movementIntent=false end
    s.transportCompleted=o.transportCompleted
  end
  if s.routeWasActive and not Flag(o.routeActive) then
    s.repathAt=0;s.meleeAdvanceAt=0
  end
  s.routeWasActive=Flag(o.routeActive)
  if not s.initialized and not Flag(o.forcedMoving) and
     not Flag(o.transportMotorReserved) then
    s.initialized=true
    -- Keep the game's initial terrain/action setup once per spawn. Subsequent
    -- decisions are generic and do not read the original script's shared globals.
    SDGO_BattleAI_Native(1)
    SDGO_BattleAI_EnableNavigationJump()
  end
  local protected=ProtectedTarget(o,s)
  local protectedHeld=protected and Flag(o.meleeBusy) and
    (protected.id==s.target or protected.id==o.nativeTarget)
  if protectedHeld then s.protectedMeleeGate=true;s.weave=nil end
  if s.protectedMeleeGate and not Flag(o.meleeBusy) then s.protectedMeleeGate=nil end
  local t=PickTarget(o,s,now)
  local releasing;t,releasing=Tactics.obstacle(npc,o,s,t,now)
  if releasing then return 123 end
  if t and s.exploring then
    -- A fresh contact replaces public exploration in this decision. Keep a
    -- safe native motor running through that goal handoff, especially when
    -- shared vision reappears during recovery; stop/start adds another native
    -- braking interval before the newly selected pursuit can move.
    SDGO_BattleAI_CancelRoute(1);s.exploring=nil
  end
  if t then o.policyTarget=t.id end
  Tactics.diagnostic(o,s,t,now)
  if Tactics.transform(o,s,t or protected,now) then return 123 end
  if Tactics.profile(npc,o,now,
       (s.weave~=nil and not Flag(o.hurt)) or Flag(o.forcedMoving) or
       Flag(o.transportMotorReserved)) then
    s.weave=nil;s.cover=nil;s.recoverUntil=nil;s.recovering=nil;s.movementIntent=false;s.anchor=o.pos;s.progressAt=now
    RecoveryMode(false);return 123
  end
  if o.hp<=0 or Flag(o.hurt) then
    -- Native hurt/capture/get-up owns both controllers. Repeating message
    -- 0x41 (attack-off) or 0x3D (MoveStop) is unnecessary controller writing
    -- here; discard only our route and decision bookkeeping.
    s.movementIntent=false;s.readyAt=nil; s.recoverUntil=nil
    SDGO_BattleAI_CancelRoute()
    s.weave=nil;s.cover=nil;MeleeGate(false)
    s.recovering=nil;RecoveryMode(false)
    return 0
  end
  if Flag(o.forcedMoving) or Flag(o.transportMotorReserved) then
    -- The selected map action owns the motor from the exact submitted request
    -- through native travel and its measured landing. Retain ordinary attack
    -- admission without replacing that movement with a route, jump or strafe.
    SDGO_BattleAI_CancelRoute();s.exploring=nil;s.movementIntent=false
    s.recoverUntil=nil;s.recovering=nil;RecoveryMode(false)
    local fire=false
    if t then
      for _,weapon in pairs(o.weapons) do
        if weapon.slot==o.selectedSlot and InRange(weapon,t) then
          fire=(Flag(weapon.ready) or Flag(o.rangedBusy)) and
            (not Flag(o.transportLanding) or Flag(weapon.movingShot))
        end
      end
    end
    SDGO_BattleAI_AimTarget(fire and t.id or 0)
    SafeSetFire(npc,o,fire);SDGO_BattleAI_Cadence(0)
    Log(s,'map action owns the motor; keep legal attack output',now)
    return 123
  end
  if not t and not s.noTargetLast then
    -- Discard the old chase on the transition to no usable target. Subsequent
    -- patrol/protected-target positioning still participates in stuck checks.
    SDGO_BattleAI_CancelRoute();s.movementIntent=false;s.anchor=o.pos;s.progressAt=now
  end
  s.noTargetLast=not t
  if Recover(npc,o,s,t or protected,now) then
    s.cover=nil;s.flanking=false;s.orbUntil=nil
    if t then Tactics.bind(npc,o,s,t,now) end
    local nextWeapon=nil
    -- Recovery owns the motor even when the native observation intentionally
    -- masks defenceMoving. Retry a ranged request on each legal recovery
    -- decision until the actual binding changes, keeping the bounded step.
    if t then
      nextWeapon=PickWeapon(o,s,t,true,now,'ranged');MeleeGate(true)
    end
    local fire=false
    for _,w in pairs(o.weapons) do
      if Ordinary(w) and w.slot==o.selectedSlot and t and InRange(w,t) and
         Flag(w.movingShot) and w.kind~='melee' then
        fire=Flag(w.ready) or Flag(o.rangedBusy)
      end
    end
    SDGO_BattleAI_AimTarget(fire and t.id or 0)
    SafeSetFire(npc,o,fire);SDGO_BattleAI_Cadence(0)
    if nextWeapon and nextWeapon.slot~=o.selectedSlot then ChangeWeapon(npc,nextWeapon.slot) end
    return 123
  end
  if s.weave or Flag(o.meleeBusy) then
    for _,held in pairs(o.targets) do
      if held.id==s.target and TargetAlive(held) and not DamageProtected(held) and Flag(held.attackable) and
         (AnyVisible(held) or Flag(o.meleeBusy)) then t=held end
    end
  end
  if not t then
    s.readyAt=nil; s.target=nil
    s.weave=nil;s.cover=nil
    if protected then
      if s.exploring then SDGO_BattleAI_CancelRoute();s.exploring=nil end
      s.noOpponentIdle=nil
      MeleeGate(Flag(s.protectedMeleeGate));PositionForProtected(npc,o,s,protected,now)
      return 123
    end
    SafeSetFire(npc,o,false)
    SDGO_BattleAI_AimTarget(0)
    MeleeGate(Flag(s.protectedMeleeGate))
    local liveEnemy=false
    for _,enemy in pairs(o.targets) do if TargetAlive(enemy) and Flag(enemy.enemy) then liveEnemy=true end end
    if not Flag(o.meleeBusy) and not Flag(o.specialBusy) then
      if not Flag(o.rangedBusy) then npc:SetAttackTarget(0) end
      if liveEnemy then
        s.noOpponentIdle=nil
        Tactics.motion(o)
        local admitted=SDGO_BattleAI_ExploreMap()==1
        s.exploring=admitted;s.patrolling=false
        s.movementIntent=true;s.movementIntentAt=now
      else
        if s.exploring then SDGO_BattleAI_CancelRoute();s.exploring=nil end
        if not s.noOpponentIdle and
           (Flag(o.moving) or Flag(o.routeActive) or s.patrolling) then
          npc:MoveStop()
        end
        s.noOpponentIdle=true;s.patrolling=false
        s.movementIntent=false
      end
    end
    Log(s,liveEnemy and 'search public map routes for an unseen opponent' or 'no remaining opponent',now)
    return 123
  end
  s.patrolling=false;s.noOpponentIdle=nil
  Tactics.bind(npc,o,s,t,now)
  local forced,waiting=nil,false
  if s.protectedMeleeGate and Flag(o.meleeBusy) then
    -- Suppress future combo continuation without forcing the current native
    -- melee action through the stock Neutral transition.
    MeleeGate(true)
  else
    if not Flag(t.mapObstacle) then forced,waiting=Weave(npc,o,s,t,now) end
  end
  if waiting then Log(s,'melee weave: release before finisher',now);return 123 end
  if Flag(t.rangedFiring) or Flag(o.hurtRecently) then s.threatUntil=now+C.ThreatMemoryMs end
  local threat=now<(s.threatUntil or 0)
  if Flag(o.rangedBusy) and (forced~='melee' or not Flag(o.moveCancelOpen)) then
    -- A charged/continuous native ranged state may already have consumed its
    -- round. Do not switch weapons, pulse its held input or cut it at a burst boundary.
    for _,active in pairs(o.weapons) do
      if Ordinary(active) and active.slot==o.selectedSlot and Flag(active.valid) then
        local keep=InRange(active,t)
        SDGO_BattleAI_AimTarget(keep and t.id or 0)
        local covered,peek=false,false
        local approaching=false
        if not Flag(o.defenceMoving) then approaching=MeleeApproach(npc,o,s,active,t,now) end
        local meleeOpportunity=MeleeOpportunity(o,t)
        local roleOwns=approaching or meleeOpportunity
        if roleOwns then
        elseif not Flag(o.defenceMoving) then covered,peek=CoverStep(npc,o,s,active,t,now) end
        if roleOwns then
        elseif covered then keep=keep and peek
        elseif Flag(o.defenceMoving) then
        elseif Flank(npc,o,s,active,t,now) then
        else
          local walked,pause=Orbwalk(npc,o,s,active,t,now)
          if walked then keep=keep and not pause
          elseif not GroundStrafe(npc,o,s,active,now,keep) then
            SDGO_BattleAI_CancelRoute()
            if keep then
              if C.Difficulty==1 and not Flag(active.movingShot) then npc:MoveStop();s.movementIntent=false
              else Evade(npc,o,s,active,t,now,true) end
            end
          end
        end
        if meleeOpportunity then
          -- Stop future ranged pulses without sending the stock attack-off
          -- message: that message queues Neutral and cuts the current action.
          -- The native ranged FSM finishes naturally; the next free decision
          -- can commit the requested melee weapon.
          ReleaseHeldIntent()
          SDGO_BattleAI_Cadence(0)
          Log(s,'melee role: release held ranged intent for natural finish',now)
          return 123
        end
        SafeSetFire(npc,o,keep)
        SDGO_BattleAI_Cadence(C.AttackIntervalMs)
        Log(s,'native ranged state owns held input',now)
        return 123
      end
    end
    return 123
  end
  local selectionKind=forced
  if Flag(o.rangedResponse) then
    selectionKind='ranged'
  end
  if not selectionKind and MeleeOpportunity(o,t) then selectionKind='melee' end
  local w=PickWeapon(o,s,t,threat,now,selectionKind)
  if selectionKind=='ranged' then
    -- A held knife tracking state can otherwise retain the native chase
    -- forever while the defensive motor retreats. Request its existing
    -- tracking-to-Neutral release only after a usable gun is known.
    -- PickWeapon returns only weapons that passed Usable/WeaponScore.
    MeleeGate(true,w and w.kind~='melee')
  end
  if not w then
    if Flag(o.meleeBusy) or Flag(o.specialBusy) then
      Log(s,'weapons unavailable: current native action retains ownership',now)
      return 123
    end
    SafeSetFire(npc,o,false); SDGO_BattleAI_AimTarget(0)
    if Flag(o.defenceMoving) then
      Log(s,'weapons unavailable: retain defensive escape route',now)
      return 123
    end
    if Flag(o.meleePressure) then
      DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE)
    else Move(npc,o,s,t,now,false) end
    Log(s,'weapons unavailable: keep moving while native resources recover',now); return 123
  end
  local pendingSlot=nil
  if o.selectedSlot~=w.slot then
    if Flag(o.meleeBusy) or Flag(o.specialBusy) then return 123 end
    npc:SetAttackOnOff(false)
    if s.requestedSlot~=w.slot then s.switchLeft=not s.switchLeft end
    pendingSlot=w.slot
    s.requestedSlot=w.slot
    local current=nil
    for _,candidate in pairs(o.weapons) do
      local permitted=Ordinary(candidate) and
        not (selectionKind=='ranged' and candidate.kind=='melee')
      if permitted and candidate.slot==o.selectedSlot and InRange(candidate,t) then current=candidate end
    end
    if current then
      -- A pending switch is not a committed weapon. Keep legal output from
      -- the actual bound slot instead of silencing the entire controller.
      w=current
    else
      if not Flag(o.defenceMoving) and not s.weave then
        if selectionKind=='ranged' and Flag(o.meleePressure) then
          DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE)
        else Move(npc,o,s,t,now,false) end
      end
      s.readyAt=nil;SDGO_BattleAI_AimTarget(0)
      ChangeWeapon(npc,pendingSlot)
      Log(s,'native switch pending: approach with current weapon',now)
      return 123
    end
  end
  if s.slot~=w.slot then
    s.slot=w.slot; s.weaponAt=now+C.WeaponHoldMs; s.readyAt=nil
    if not pendingSlot then s.requestedSlot=nil end
  end
  local inRange=InRange(w,t)
  local reaction=inRange and Flag(w.ready)
  if Flag(o.meleeBusy) or Flag(o.specialBusy) then return 123 end
  SDGO_BattleAI_AimTarget(inRange and t.id or 0)
  if Flag(o.defenceMoving) then
    npc:SetAttackOnOff(reaction);SDGO_BattleAI_Cadence(0)
    if pendingSlot then ChangeWeapon(npc,pendingSlot) end
    Log(s,'front/rear crossfire: keep defensive movement and fire',now)
    return 123
  end
  local roleApproaching=MeleeApproach(npc,o,s,w,t,now)
  if selectionKind=='melee' and pendingSlot then
    npc:SetAttackOnOff(reaction);SDGO_BattleAI_Cadence(C.AttackIntervalMs)
    ChangeWeapon(npc,pendingSlot)
    Log(s,'melee opportunity: retain legal bound output while switching',now)
    return 123
  end
  if roleApproaching then
    npc:SetAttackOnOff(reaction);SDGO_BattleAI_Cadence(C.AttackIntervalMs)
    if pendingSlot then ChangeWeapon(npc,pendingSlot) end
    return 123
  end
  if Tactics.finishBoost(o,s,w,inRange) then
    npc:SetAttackOnOff(reaction);SDGO_BattleAI_Cadence(C.AttackIntervalMs)
    if pendingSlot then ChangeWeapon(npc,pendingSlot) end
    Log(s,'finish native boost startup before the next firing or movement decision',now)
    return 123
  end
  local covered,peek=CoverStep(npc,o,s,w,t,now)
  if covered then
    npc:SetAttackOnOff(reaction and peek and inRange)
    SDGO_BattleAI_Cadence(C.AttackIntervalMs)
    if pendingSlot then ChangeWeapon(npc,pendingSlot) end
    return 123
  end
  if not inRange then
    npc:SetAttackOnOff(false)
    Move(npc,o,s,t,now,false)
    if pendingSlot then ChangeWeapon(npc,pendingSlot) end
    Log(s,'approach through navigation',now)
    return 123
  end
  local flanking=Flank(npc,o,s,w,t,now)
  local walked,pause=false,false
  if not covered and not flanking then walked,pause=Orbwalk(npc,o,s,w,t,now) end
  if covered then reaction=reaction and peek end
  if pause then reaction=false end
  local evasive=false
  local ground=false
  if not covered and not flanking and not walked then
    local distance=TargetDistance(w,t)
    local tooClose=w.kind~='melee' and distance<w.range*0.18
    if not tooClose then ground=GroundStrafe(npc,o,s,w,now,reaction) end
    if not ground then
      SDGO_BattleAI_CancelRoute()
      evasive=Evade(npc,o,s,w,t,now,reaction)
    end
  end
  if not covered and not flanking and not walked and not evasive and not ground then
    local distance=TargetDistance(w,t)
    if w.kind~='melee' and distance<w.range*0.18 then
      DirectMove(npc,o,UNIT_MOVE_DIR_BACKWARD,Min(C.StrafeDistance,w.range*0.2),UNIT_MOVE_EVENT_MOVE)
    elseif not reaction then
      -- Spend native cooldown/reload time positioning, rather than adding an
      -- idle state. A ready stationary-only weapon stops and fires immediately.
      if not s.cooldownMoving or not Flag(o.moving) then
        s.cooldownLeft=not s.cooldownLeft
        local dir=s.cooldownLeft and UNIT_MOVE_DIR_LEFT or UNIT_MOVE_DIR_RIGHT
        DirectMove(npc,o,dir,C.StrafeDistance,UNIT_MOVE_EVENT_MOVE)
      end
      s.cooldownMoving=true
    else s.cooldownMoving=false;s.movementIntent=false;npc:MoveStop() end
  end
  -- Intent only: native animation, ammo, weapon cooldown, projectile generation
  -- and collisions execute the attack. No synthesized hit/damage packet.
  npc:SetAttackOnOff(reaction)
  SDGO_BattleAI_Cadence(C.AttackIntervalMs)
  if pendingSlot then ChangeWeapon(npc,pendingSlot) end
  Log(s,reaction and 'attack intent' or 'native weapon unavailable: position for next attack',now)
  return 123
end
function SDGO_BattleAI_ProfileRelease(npc)
  local slot=SDGO_BattleAI_ProfilePreviousSlot and SDGO_BattleAI_ProfilePreviousSlot() or -1
  local identified,id=pcall(npc.GetID,npc)
  if not identified or id==nil then id=tostring(npc) end
  if not U.releases then U.releases={} end
  local function Release()
    U.releases[id]=nil
    if U.profiles then U.profiles[id]=nil end
    if SDGO_BattleAI_ProfileLease then SDGO_BattleAI_ProfileLease(-1) end
  end
  local selected,mask,busy=-1,-1,0
  if SDGO_BattleAI_BoundSlot then selected,mask,busy=SDGO_BattleAI_BoundSlot() end
  if Flag(busy) then U.releases[id]=nil;return 123 end
  local valid=mask==nil or mask<0 or
    (slot==0 and (mask==1 or mask==3 or mask==5 or mask==7)) or
    (slot==1 and (mask==2 or mask==3 or mask==6 or mask==7)) or
    (slot==2 and mask>=4)
  if slot<0 or slot>=3 or not valid or selected==slot then Release();return 123 end
  local now=SDGO_BattleAI_Now()
  local generation=SDGO_BattleAI_Generation and SDGO_BattleAI_Generation() or 0
  local pending=U.releases[id]
  if not pending or pending.slot~=slot or pending.generation~=generation then
    pending={slot=slot,generation=generation,started=now};U.releases[id]=pending
  end
  if now-pending.started>=250 then Release();return 123 end
  npc:SetAttackOnOff(false);ChangeWeapon(npc,slot)
  return 123
end
function SDGO_BattleAI_ProfileUpdate(npc)
  if C.Enabled~=1 or C.Difficulty~=1 or SDGO_BattleAI_Authority()~=1 then return 123 end
  local ok,result=pcall(function() return ProfileTick(npc,SDGO_BattleAI_Observe(),SDGO_BattleAI_Now()) end)
  if not ok then SDGO_BattleAI_Log('profile action unavailable: '..tostring(result)) end
  return 123
end
function SDGO_BattleAI_GenericUpdate(npc)
  if C.Difficulty<1 or C.Enabled~=1 or C.GenericMode~=1 or SDGO_BattleAI_Authority()~=1 then
    return SDGO_BattleAI_Native()
  end
  local identified,id=pcall(npc.GetID,npc)
  if not identified or id==nil then id=tostring(npc) end
  local generation=SDGO_BattleAI_Generation and SDGO_BattleAI_Generation() or 0
  local failure=U.failures and U.failures[id]
  local ok,result=pcall(Tick,npc)
  if ok then
    if U.failures then U.failures[id]=nil end
    return result
  end
  U.faults=U.faults+1
  if not U.failures then U.failures={} end
  if not failure or failure.generation~=generation then failure={generation=generation,count=0};U.failures[id]=failure end
  failure.count=failure.count+1
  local now=SDGO_BattleAI_Now()
  if failure.count<=3 or not failure.loggedAt or now-failure.loggedAt>=5000 then
    SDGO_BattleAI_Fault('NPC '..tostring(id)..' generation '..tostring(generation)..': '..tostring(result))
    failure.loggedAt=now
  end
  ReleaseHeldIntent()
  return 123
end
U.faults=0
U.failures={}
U.difficulty=C.Difficulty
SDGO_BattleAI_Universal=U
