#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>
#include <stdint.h>
#include <string>
#include <mutex>
#include <algorithm>

// ---- logging / config (main.cpp) ----
void Log(const char* fmt, ...);
std::wstring ModDir();
void ConfigReloadIfChanged();   // re-reads twfcvr.ini when it was saved (tuning while playing)

struct Config {
    float fpsLimit = 120.0f;        // 0 = leave game default
    float vrFpsLimit = 300.0f;      // cap while the headset session runs (the headset paces the game)
    int   vrEnabled = 1;
    int   vrMode = 1;               // 0 = theater (virtual screen) always, 1 = stereo 3D in gameplay
    float worldScale = 200.0f;       // Unreal units per real metre for head movement
    float fovScale = 1.0f;          // >1 renders a wider frustum than the headset needs
    int   renderWidth = 1800;       // game resolution override for VR (0 = game setting)
    int   renderHeight = 1800;
    int   aimMode = 1;              // 0 = aim where you look, 1 = aim with the right controller
    int   vrHud = 1;                // game HUD on a floating panel instead of painted into the eyes
    float hudDistance = 1.6f;       // metres
    float hudWidth = 1.5f;          // metres
    float hudFollowSpeed = 3.0f;    // how fast the panel catches up with the gaze (1/s)
    float hudCrosshairMask = 0.04f; // radius (fraction of HUD width) hidden at the centre: the game's crosshair
    int   hudMinimal = 1;           // HUD panel shows only health, abilities, weapon and ammo
    int   hudPrompts = 1;           // ... and the button hints ("Hold X for ...") below the centre
    int   reticle = 1;              // VR crosshair along the controller ray
    float reticleDistance = 8.0f;   // metres
    float reticleSize = 0.6f;       // the ring's size (x the default, ~3 degrees; the game's crosshair: x its own size)
    int   reticleStyle = 1;         // 1 = the game's own crosshair on the VR reticle, 0 = a plain ring
    float beltOffset[3] = { -0.16f, -0.55f, 0.12f };   // grenade pouch from the eyes: right, up, forward (m)
    int   stereoPerFrame = 1;      // stereo: each game frame is drawn twice, once per eye (0 = one eye per frame, alternating)
    int   syncEyeTime = 1;          // alternating stereo: the right-eye frame shows the same moment as the left
    int   syncEyeTimeVehicle = 1;   // ... also in vehicle form
    double heldFrameTime = 0.001;   // the game time a held (right-eye) frame still advances, seconds
    int   autoScale = 1;            // WorldScale from the robot's arm span and the player's (T-pose in the setup)
    float userArmSpan = 0.0f;       // the player's arm span, wrist to wrist (metres; 0 = not calibrated)
    float emptyGripR[7] = { 0, 0, 0, 1, 0, 0, 0 };   // the empty right hand on the controller (setup; qx qy qz qw tx ty tz)
    float userEyeHeight = 0.0f;    // the player's eye height standing (metres; T-pose; 0 = not calibrated)
    int   singleThreadRender = 1; // the engine renders on the game thread: each image is matched to its eye exactly
    int   handIK = 1;               // first person: the robot's arms and weapon follow the controllers
    int   meleeGesture = 1;         // a fast swing of a hand is a melee attack
    float meleeSpeed = 3.0f;        // hand speed (m/s) that counts as a swing
    int   meleeContact = 0;       // ... and strikes only when the weapon reaches another robot (melee.cpp)
    float meleeReach = 0.55f;       // the melee weapon's length from the hand (metres)
    float meleeHitRadius = 90.0f;   // a robot's body for the weapon: radius (Unreal units; height twice that each way)
    int   throwGesture = 1;         // a throw with the free left hand throws a grenade
    float throwSpeed = 2.5f;        // hand speed (m/s) that counts as a throw
    int   gripUse = 1;              // a free hand's grip away from the body = the game's use button (X)
    int   grenadeGrab = 1;         // grenades taken from the body with the left hand, armed, thrown (replaces the gesture)
    float grenadeThrowScale = 3.0f; // thrown grenade speed = hand speed x this
    float grenadeGravity = 1.0f;    // a thrown grenade falls (x 9.8 m/s²; the game's own fly almost straight)
    int   hudMarkersInEyes = 1;     // minimal HUD: its other parts (waypoints, names, lock-on marks) drawn into the eyes
    int   cutscenes3D = 0;          // 1 = in-engine cutscenes in 3D around you (0 = on the virtual screen)
    int   grenadeBelt = 1;          // a grenade is seen on the belt (where they are taken from) while there are any
    float grenadeScale = 0.5f;      // size of the grenade on the belt and in the hand (1 = the game's thrown one)
    int   fingerPoses = 1;          // robot fingers follow yours on the controller (trigger, grip, thumb on the buttons)
    int   fingerPalmSign = 1;       // -1 = fingers bend the other way (if a robot's hand bends them backwards)
    int   pull = 1;                 // point the free left hand at a pickup and jerk it back: it flies to you
    float pullRange = 12.0f;        // metres
    float pullConeDeg = 15.0f;      // degrees around the free hand's ray
    float pullSpeed = 0.0f;         // m/s of a jerk back that also pulls (0 = off: point and squeeze the grip)
    int   pullAutoTake = 1;         // a pulled weapon is taken (the interact button held for it) when it arrives
    int   pullTrace = 1;            // only pickups in sight (not through walls)
    int   twoHanded = 1;            // left grip near the weapon's front = hold it with both hands
    float twoHandReach = 0.25f;     // metres in front of the right controller where the left hand grabs
    float twoHandRadius = 0.15f;    // how close the left hand must be to that point
    int   physicalAim = 0;          // weapon held two-handed and raised to the eyes = aiming (the game's FineAim)
    int   gripAbilities = 0;        // 1 = old layout: grips are the ability buttons (LB/RB), Y = next weapon
    float wheelSize = 0.30f;        // weapon wheel diameter (metres)
    float handOffset[3] = { 0, 0, 0 };   // robot hands moved by this (cm, body frame: forward, right, up)
    float gunGrip[7] = { 0, 0, 0, 1, 0, 0, 0 };   // the gun in the right hand: turn (quaternion) and shift (m), aim axes
    float foregrip[3] = { 0, 0, 0 };     // two-handed: the left hand's place, relative to the right grip (m, aim axes)
    int   foregripSet = 0;               // 0 = taken from the gun's model
    int   scope = 0;                // 1: sniper rifles raised to the eye / held two-handed show a magnifying lens (off: only the main crosshair)
    float scopeFov = 8.0f;          // degrees of view shown in the scope
    float scopeSize = 0.07f;        // lens diameter (metres)
    float scopeForward = 0.10f;     // lens position: metres in front of the right controller
    float scopeUp = 0.05f;          //   and above its aim ray
    wchar_t scopeWeapons[128] = L"Sniper,NullRay,Eliminator";   // weapon name parts that get a scope ("*" = all)
    float abilityYield = 0.0f;     // seconds the arms play the animation after an ability button (0 = never)
    float noNearFade = 600.0f;      // first person: robots closer than this (Unreal units) are not faded out (0 = game fades)
    float nearClip = 10.0f;         // near clipping plane (Unreal units; game uses 100 = 0.5 m at VR scale)
    int   showBody = 1;             // first person: 0 = arms only, 1 = arms + legs (torso hidden), 2 = whole body
    float limbThickness = 0.5f;     // first person: upper arms drawn thinner (1 = original), on top of armSize
    float armSize = 0.8f;           // first person: thickness of the whole arms - forearms, armour, arm shell (1 = original)
    float legThickness = 0.75f;     // first person: legs drawn thinner (1 = original)
    float handScale = 1.0f;         // first person: size of the robot's hands
    float weaponScale = 1.0f;       // first person: size of the held weapon
    float muzzleFlashScale = 0.35f; // first person: size of the weapon's muzzle flash effects
    int   english = 1;              // the mod's own text: 1 English, 0 Russian ([General] Language=auto|en|ru)
    int   bodyFollowHead = 1;       // first person: the body stays under the head when you walk around the room
    int   moveByHead = 1;           // left stick moves where the head looks (0 = where the right controller aims)
    int   vehicleCamera = 1;        // vehicle form: 0 = driver's seat, 1 = chase camera behind and above
    float chaseDistance = 450.0f;   // chase camera: distance behind (Unreal units)
    float chaseHeight = 260.0f;     // chase camera: height above the vehicle origin
    int   vehicleHeadAim = 0;       // vehicle form: 0 = aim (and steer) with the right controller, 1 = where the head looks
    float vehicleEyeHeight = 210.0f;  // vehicle form: eye height above the pawn origin
    float vehicleEyeForward = -100.0f; // vehicle form: eye offset forward (negative = behind)
    int   cameraMode = 1;       // 0 = third person (game camera), 1 = first person (at the pawn's eyes)
    float eyeHeight = 150.0f;     // first person: eye height above the pawn origin (Unreal units)
    float eyeForward = 30.0f;       // first person: eye offset forward of the pawn origin
    float turretEyeUp = 0.35f;      // on a mounted turret: eyes (and body) raised by this much (metres, real) to see over it
    int   keepStickPitch = 0;       // 1 = keep the game camera pitch under the head pitch
    int   snapTurnDeg = 0;              // >0 = snap turn by this many degrees, 0 = smooth turn
    float smoothTurnDegPerSec = 120.0f;
    int   ownTurning = 1;               // main mode: the mod turns the body (game turning/aim assist cannot move the view)
    unsigned menuCameraVTable = 0x1293D590;   // view target class of the main menu backdrop
    float screenDistance = 3.0f;    // metres
    float screenWidth = 4.0f;       // metres
    float screenHeight = 1.6f;      // metres above floor/eye origin offset
    int   vrControllers = 1;        // feed VR controllers into XInput
    float stickDeadzone = 0.15f;
    int   alwaysFocused = 1;        // game keeps input/rendering when its window is not in front
    int   blockMouseKeyboard = 1;   // headset on: the game gets no mouse/keyboard
    int   buttonHints = 1;          // hints name the VR controller buttons instead of keyboard keys (hints.cpp)
    int   debugAutomation = 0;      // twfcvr_cmd.txt drives a virtual pad
    int   logFps = 0;
    int   probeCamera = 0;
    int   fakeHmd = 0;              // synthetic head pose for testing without a headset
    int   logCameraSeq = 0;         // debug: mark N camera updates in the log
};
extern Config g_cfg;

// ---- game.cpp ----
bool GameIsSupportedBuild();
void GameStartPatchThread();
bool PullGrab(int hand);                     // pull.cpp: a new squeeze of that hand - pulls what it points at (true)
bool PullWantInteract();                     // a pulled weapon arrived: hold the interact button (X)
bool VRCameraAdjusting();                    // the menu's camera placement is on (the camera follows without a glide)
int VRGunHand();                            // the hand holding the gun: 1 right, 0 left (mirrored), -1 put away
bool VRRightEmpty();                       // the right hand was chosen empty (gun put away, free hand)
bool MeleeContact(int hand, bool swinging, DWORD now);   // melee.cpp: the swung weapon reached a robot this frame
bool MeleeTargetYaw(int& yaw);               // ... the world yaw to it, for a moment after the hit
void MeleeDebugTarget(void** target, float* health);
void MeleeDebug(int what, void* actor);      // debug: 1 = log the robots, 2 = bring that one in front of the hand
bool IKHandWorld(int hand, float pos[3], float fwd[3], float rel[3]);   // ik.cpp: a hand in the world (Unreal units)
float IKRobotEyeHeight();                    // ik.cpp: the robot's eye height above its feet, world units
float IKRobotArmSpan();                     // ik.cpp: the robot's arm span, world units (0 = not measured yet)
void* IKPawn();                          // ik.cpp: the player's pawn in first person (null otherwise)
void GameEarlyPatches();                   // DllMain: rendering on the game thread (SingleThreadRender)
bool GameSingleThreadRender();               // the engine has no render thread: presents happen inside the game tick
DWORD GameThreadId();                        // thread ID of the engine's main/game loop

struct Vec3f { float x, y, z; };

// UE3 camera point of view as laid out in this build (FTViewTarget+8 / CameraCache.POV).
#pragma pack(push, 4)
struct TPOV {
    float x, y, z;              // Location (Unreal units, +X forward, +Y right, +Z up)
    int pitch, yaw, roll;       // Rotation (65536 = 360 degrees)
    float fov;                  // horizontal FOV in degrees
};
#pragma pack(pop)

// ---- camera.cpp ----
void CameraOnUpdate(TPOV* pov, void* viewTarget, void* pc, float dt);   // game thread, after the engine computed the view
bool ReadActorLocation(void* actor, Vec3f& out);               // game.cpp, SEH-guarded
void GameSetAim(bool valid, int pitch, int yaw, float x, float y, float z);   // game.cpp: weapon aim, applied after the view is drawn
long GameFrameCount();
bool CameraInGameplay();                    // camera updated recently (not in menus / movies)
bool CameraPlayerView();                    // ... and it looks through the player (not a cutscene)
bool CameraGameZoom(float& fovDeg);         // the game is zoomed in (scoped fine aim); fovDeg = the game's FOV
void CameraSnapTurn(int units);
bool CameraOwnsTurning();                   // main mode: right stick turns our body yaw, hidden from the game
void CameraSetTurnInput(float x);

// ---- ik.cpp: arms follow the controllers ----
void IKInstall();
void IKSetPawn(void* pawn, const float viewPos[3]);   // first-person player pawn (null = off) and the eye position
// eye of this frame; latched head offset (body frame, horizontal), body yaw (world) and head yaw (vs body), radians
void IKSetFrame(int eye, const float headOffset[3], float bodyYaw, float headYaw);
bool IKPawnIsVehicle();                     // the player's current skeleton has no arms (vehicle form)
void IKSetBelt(void* comp, float size = 0); // a component attached to the hips, placed each frame on the belt (or null); its world size (0 = weapons')
bool IKWeaponHasScope();                    // first person, holding a weapon from ScopeWeapons
float CameraVehicleSteer();                 // vehicle form: right stick (-1..1) that turns it towards the aim
float CameraMoveYawDelta();                // radians to rotate the left stick so movement follows the head
void CameraBaseOffset(float& forward, float& up);   // eye base relative to the pawn (Unreal units)
bool CameraVehicleAim();                    // vehicle form: the weapon aims where the head looks
float CameraAimHitMeters();                 // distance to where the game's aim trace hits (0 = unknown)
void IKSetHand(int hand, bool valid, const float pos[3], const float axes[3][3]);   // body frame pose relative to the eye base; axes[k] = X/Y/Z
void IKOnAttach(void* mesh, void* comp);    // weapons.cpp: the game attached comp to mesh (after it did)
bool IKOnDetach(void* mesh, void* comp);    // weapons.cpp: the game detaches comp from mesh (false = keep it attached)
bool IKArmModelHeld(void* mesh, void* comp);   // weapons.cpp: comp is the arm model the mod keeps on the empty right hand
void IKArmModelUpdate(void* pawn, bool allowed);   // weapons.cpp, game thread, each frame: the arm model on the empty right hand

// ---- weapons.cpp: the player's weapons (weapon wheel) and the melee weapon held in a hand ----
struct WeaponList {
    int count = 0;
    int current = -1;            // index of the weapon in hand (-1 = unknown)
    wchar_t name[8][40] = {};
};
void WeaponsInstall();
void WeaponsUpdate(void* pawn);             // game thread, once per frame (null = not in first person)
void WeaponsGetList(WeaponList& out);       // any thread
void WeaponsSelect(int index);              // switch to that weapon of the list (presses "next weapon" until it is in hand)
void WeaponsSetMeleeHand(int hand);         // -1 = put away, 0 = left, 1 = right hand holds the melee weapon
int WeaponsMeleeHand();                     // the hand asked for (-1 = none)
int WeaponsMeleeHeld();                     // the hand the melee weapon is in right now (-1 = none)
void WeaponsStrike(int hand);               // VR input: a melee attack by that hand starts now (the robot is held in place)
bool WeaponsWantPress(bool melee);          // VR input: the game's next-weapon (false) / melee (true) button is wanted now
bool WeaponsIsMeleeComp(void* comp);        // ik.cpp: that component is the melee weapon kept in a hand
bool WeaponsIsFlipped(void* comp);          // ik.cpp: a punch with the empty left hand moved that component to the left side
bool WeaponsIsAttached(void* mesh, void* comp);
// ik.cpp: attach/detach through the original engine functions (not seen by the hooks)
void WeaponsAttachOriginal(void* mesh, void* comp, unsigned bone, const float* loc, const int* rot, const float* scale);
void WeaponsDetachOriginal(void* mesh, void* comp);
bool WeaponsOnTurret();                     // the player is on a mounted turret (its gun in hand)
float WeaponsTurretClearance(const float o[3], const float dir[3]);   // the aim ray's run through the turret gun
bool WeaponsTurretMuzzle(const float o[3], float out[3]);            // on a turret: its muzzle (world), near o
float CameraTurretLift();                   // on a turret: how far the eyes are raised (Unreal units, gliding)

// ---- pull.cpp: pickups pulled from a distance with the left hand ----
void PullUpdate(void* pawn);                // game thread, once per frame (null = not in first person)
void PullDebug(int what, void* actor = nullptr);   // 1 = log the pickups, 2 = pull the nearest one (or actor)
bool MoveActorTo(void* actor, const float pos[3], bool noCheck);   // UWorld::FarMoveActor (collision moves along)
// the world's line check (FastTrace flags 0x2286 = world geometry; | 1 = pawns too); source actor is ignored
bool WorldLineClear(void* source, const float from[3], const float to[3], unsigned flags);

// ---- grenades.cpp ----
void GrenadesUpdate(void* pawn);            // game thread, once per frame (null = not in first person)
void GrenadeGrab(int hand);                 // VR input: a free hand (0 left, 1 right) took one from the pouch
int GrenadeHand();                           // the hand holding one, -1 if none
void GrenadeArm();                          // ... armed it (left trigger)
void GrenadeRelease();                      // ... let go
bool GrenadeHeld();                         // one in the left hand (or on its way)
bool GrenadeWantPress();                    // press the game's grenade button now (once)
bool GrenadeWeaponClass(const wchar_t* cls); // the thrown kinds: TnWeaponFlakGrenades, ...KamikazeMines, ...EmpGrenades
unsigned IKMirrorBone(unsigned boneName);   // ik.cpp: the other side's bone name of the player's skeleton (0 = none)

// ---- vr.cpp: stereo frame pairing ----
struct VREyeView {
    int eye;                     // 0 = left, 1 = right
    float px, py, pz;            // eye position in LOCAL space (OpenXR, metres)
    float qx, qy, qz, qw;        // eye orientation (OpenXR)
    float renderFovDeg;          // horizontal FOV the game must render with
    struct Hand {
        bool valid;
        float px, py, pz;        // aim pose in LOCAL space (OpenXR)
        float qx, qy, qz, qw;
    } aim[2], grip[2];           // 0 = left, 1 = right controller (aim = pointing ray, grip = where the hand holds it)
};
void VRBeginTick();                          // game thread: begins the OpenXR frame + input before the tick measures time
bool VRBeginGameFrame(VREyeView& out);      // game thread: picks the eye for this frame
bool VRBeginStereoPair(VREyeView& left, VREyeView& right);   // game thread: both eyes, the frame is drawn twice
void GameSetRightEyeView(const TPOV& pov);  // game.cpp: the view of the second drawing of this frame
bool VRNextStereoEye(int& eye);             // eye of the next stereo frame; false when no stereo views lately
bool VRTwoHanded();                         // the weapon is held with both hands
void VRFingerCurls(int hand, float out[4]); // empty hand: index, middle, ring, thumb bend (0..1) from the controller
bool VRTwoHandTurn(float q[4]);             // held two-handed: turn (quaternion x,y,z,w, OpenXR LOCAL) of the right hand's aim
// a mounted turret's aim from both hands: the orientation between the two aim poses, from the point between them
void VRTurretAim(const VREyeView::Hand& l, const VREyeView::Hand& r, float q[4], float p[3]);
bool IKForegrip(float out[3]);              // ik.cpp: held weapon's front grip relative to the right aim pose (OpenXR axes, metres)
bool VRMeleeSwing(float& x, float& z);      // recent melee swing direction (OpenXR horizontal)
DWORD VRAbilityPressedAt();                 // GetTickCount of the last ability (bumper) press
void VRHaptic(int hand, float amplitude, float seconds);
bool GameTimeFrozenForRightEye();           // game.cpp: the right-eye frames do not advance game time
bool VRIsRunning();
bool VRIsRunningOrFake();

// ---- registry.cpp / d3d9modes.cpp ----
void RegistryInstallHooks();
void D3D9ModesInstall();

// ---- focus.cpp / inputblock.cpp ----
void FocusStart();
void InputBlockInstall();

// ---- hints.cpp ----
void HintsInstall();

// ---- game.cpp (movies) ----
void GameInstallMovieHook();
bool GameMoviePlaying();                    // a Bink movie is playing (shown on the virtual screen)
void WindowEarlyInit();

// ---- debug.cpp ----
void DebugStart();
bool DebugGetPad(XINPUT_GAMEPAD& out);

// ---- vr.cpp ----
void VRStart();   // installs present hooks from a worker thread

// Latest VR controller state mapped to an Xbox pad (written by the VR thread, read by XInput proxy).
struct VRPadState {
    bool active = false;
    XINPUT_GAMEPAD pad = {};
};
void VRGetPad(VRPadState& out);

