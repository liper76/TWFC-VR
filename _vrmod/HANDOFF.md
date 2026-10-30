# VR-мод для Transformers: War for Cybertron — передача проекта

Документ для продолжения работы другой нейросетью (или человеком). Составлен 29.10.2026 по итогам ~5 дней работы.
Подробная хронология со всеми находками и проверками — `_vrmod\PROGRESS.md` (большой, читать последние разделы).
Инструкция для игрока — `VR-мод — инструкция.txt` в корне игры.

---

## 1. Что это и чего хочет пользователь

- Полноценный VR-мод для **Transformers: War for Cybertron (2010, PC, Unreal Engine 3, 32-bit, DirectX 9)**.
  Шлем пользователя работает через **Virtual Desktop** (OpenXR-рантайм VD), контроллеры Quest (Touch).
- Приоритет — **геймплей**: от первого лица, руки робота по контроллерам (IK), прицеливание контроллером,
  жесты (удар, гранаты как в Half-Life: Alyx, притягивание предметов как в Boneworks), колесо оружия, свой HUD.
- Пользователь говорит по-русски, тестирует в шлеме сам и присылает отзывы (часто с видео/скриншотами).
  Без шлема мод проверяется на фейковом OpenXR-рантайме **MockXR** (см. §4).

### Правила пользователя (соблюдать строго)
1. **Никогда не трогать оригинальную папку `Binaries\`** (там ReShade + DLSS + dgVoodoo для обычной игры).
   Мод живёт в `BinariesVR\` (свой TWFC.exe + DLL мода). Классический (старый) режим — `BinariesVR_Classic\`.
2. Можно закрывать и запускать игру из `BinariesVR` для тестов. Если игра запущена пользователем — не закрывать.
3. Перед тестами бэкапить сейвы (`Documents\My Games\Transformers\TransGame\SaveData`, бэкапы в `_vrmod\backup\`)
   и не менять их. После теста сверять хэши.
4. Перед уходом: в `BinariesVR\twfcvr.ini` — `Automation=0`, без `FakeHmd`, без тестовых ключей/секций.
5. Не делать скриншоты рабочего стола — только окна игры (`_vrmod\tools\shotgame.ps1`), удалять после.
6. **Никаких фоновых/запланированных задач** (все отключены по просьбе пользователя: «делаем только то, что видно»).
7. Не трогать реестровое разрешение экрана игры (ScreenResolution).
8. PowerShell 5.1 портит русский UTF-8 через Get-Content/Set-Content — использовать редактор файлов или
   `[IO.File]::ReadAllText/WriteAllText` с UTF-8.
9. Плавающую отстающую HUD-панель (основной интерфейс) без просьбы не переделывать.
10. Честно писать, что проверено (MockXR / в шлеме), а что нет.

---

## 2. Рабочая область

```
C:\Games\Transformers - War for Cybertron\
  Binaries\                 ОРИГИНАЛ (не трогать)
  BinariesVR\               игра с модом: TWFC.exe (флаг LARGE_ADDRESS_AWARE; копия TWFC.exe.noLAA),
                            xinput1_3.dll (= мод), openxr_loader.dll, dgVoodoo (D3D9.dll и др.), dgVoodoo.conf,
                            twfcvr.ini (настройки мода), twfcvr.log (лог мода, перезаписывается при запуске),
                            twfcvr_cmd.txt (команды автоматизации)
  BinariesVR_Classic\       старая версия мода (3-е лицо), снимок _vrmod\snapshots\classic_2026-10-25
  "Запуск VR.bat", "Запуск VR (классика).bat"   ярлыки запуска для пользователя
  VR-мод — инструкция.txt   инструкция игрока (управление, ini)
  _vrmod\
    src\                    исходники мода (C++)
    build.bat               сборка + деплой в BinariesVR (падает «DEPLOY FAILED», если игра запущена)
    build\                  промежуточные файлы
    tools\                  python-инструменты анализа, MockXR, скрипты тестов
    backup\                 бэкапы сейвов
    snapshots\              снимок классической версии
    PROGRESS.md             журнал (всё, что сделано, найдено, проверено)
    HANDOFF.md              этот файл
    ACTIVE_SESSION.txt      «замок» сессии (удалять в конце работы)
  _vrmod_tools\
    venv\Scripts\python.exe python с pefile, capstone (для tools\*.py)
    openxr\                 заголовки OpenXR SDK
    (MinHook и прочее для сборки)
```

---

## 3. Сборка

- `_vrmod\build.bat` — MSVC (VS 2022 Build Tools, vcvars32, x86), собирает `xinput1_3.dll` из всех `src\*.cpp`
  (список файлов в самом bat: main, xinput_proxy, game, vr, debug, focus, camera, registry, d3d9modes, d3dtrace, ik,
  weapons, inputblock, pull, hints, grenades, melee), линкует MinHook, openxr_loader, d3d11, деплоит в BinariesVR.
  Новый .cpp — дописать в build.bat.
- Запуск из PowerShell: `cmd /c "<полный путь>\_vrmod\build.bat"`.
- Код: комментарии английские; русский текст в UI — только через `\x0410`-escape в wide-строках (исходники не
  в UTF-8 для компилятора). Осторожно: `\x` жадно ест следующие hex-символы (a–f, 0–9).

---

## 4. Тестирование без шлема

### MockXR (`_vrmod\tools\mockxr\`)
- Свой OpenXR-рантайм (`mockxr.dll`, сборка `build_mock.bat`). Подключается только к тестовому процессу через
  переменную `XR_RUNTIME_JSON=...\mockxr\mockxr.json`.
- Каждые 450 кадров пишет `mockxr_<кадр>_eye0/eye1/overlayN/quad.bmp` и лог `mockxr.log` (слои, позы квадов).
- Как реальный рантайм, отвергает квады с ненормированным кватернионом (`xrEndFrame` = -39 POSE_INVALID).
- Позы и кнопки задаются файлом `mockxr\mockxr_pose.txt` (перечитывается на лету):
  ```
  head x y z yaw pitch
  left_aim_pose x y z yaw pitch roll      (также right_aim_pose, *_grip_pose; yaw + = влево, pitch + = вверх)
  left_grip 1 / right_trigger 1 / button_a 1 / menu 1 / right_stick_click 1 / left_stick 0 0.8
  vel_left 0 0 -5                         скорость руки (м/с), -z = вперёд
  ```
  Имена = имена действий мода (`%s_grip`, `%s_trigger`, `%s_stick`, `button_a/b/x/y`, `left/right_stick_click`, `menu`).
  Удерживать нажатие ≥1 с, иначе можно не попасть в опрос.

### Автоматизация игры
- `[Debug] Automation=1` в ini → мод читает `BinariesVR\twfcvr_cmd.txt` (виртуальный геймпад + debug-команды):
  `press A 150`, `wait 500`, `stick LY 1 4000`, и команды мода: `weapons`, `weapon N`, `melee H` (-1/0/1),
  `wheel <hand> <slot>` / `wheel -1 0` (выбрать), `pickups`, `pullnear <hex>`, `pullaim`, `pawns`, `bringpawn <hex>`,
  `meleetarget`, `markcam N` (лог последовательности кадра), `fakehead`, `probeyaw` и др. (см. `src\debug.cpp`).
- `tools\run_vent.ps1` — бэкап сейвов, ini Automation=1, запуск BinariesVR\TWFC.exe на MockXR, пропуск заставок,
  проход меню до тестового чекпоинта (`tools\go_vent.txt`: Campaign → Chapter Select → глава I → **Ventilation
  Shafts**, Мегатрон). `-Cleanup` — вернуть ini. `-PopupWait 60` если окно Accounts появляется поздно.
  Через ~75 с после запуска — геймплей (лог: «vr: stereo gameplay view»).
- `tools\vehicle_test.ps1 "Key=Value"` — временные ключи [VR], превращение, газ 4 с, замер скорости (`speed.py`).
- Чтение/запись памяти игры: `tools\memscan.py` (Proc(find_pid('TWFC.exe')), read/u32), `tools\poke.py`
  (`poke.py <сек> <hexaddr>=<float|Ni>`), `objbyname.py <Name>` (UObject по FName), `findstrref.py`, `disasm.py
  <exe> <hexaddr> <hexlen>`, `findprocevent.py` и др. Указатель пешки — из лога (`ik: ... (pawn XXXXXXXX)`).
- На чекпоинте Ventilation Shafts: 2 напарника `TnAiPawn` (здоровье 100, огня по своим нет), ящик патронов
  `TnAmmoCratePickupFactory`; врагов, гранат и оружейных пикапов рядом нет.

---

## 5. Архитектура мода

Мод — прокси `xinput1_3.dll` (импорт игры). `DllMain` (main.cpp): лог, LoadConfig, `GameEarlyPatches()` (до старта
движка), хуки реестра/режимов/окна, затем поток `InitThread` (хуки движка, VR). Рендер: D3D9 игры → dgVoodoo →
D3D11/DXGI; мод хукает DXGI Present и отдаёт кадры в OpenXR (D3D11).

| Файл | Назначение |
|---|---|
| main.cpp | DllMain, загрузка/перезагрузка ini на лету, дефолтный ini-шаблон |
| xinput_proxy.cpp | экспорт XInput, смешивание виртуального геймпада VR с реальным |
| game.cpp | патчи движка: лимит FPS, ближняя плоскость, удержание времени для правого глаза, хуки камеры/кадра/Draw вьюпорта, однопоточный рендер, детект роликов Bink |
| camera.cpp | CameraOnUpdate (из хука UpdateViewTarget): поза головы/глаза → камера, прицел контроллером (GameSetAim), режимы машины, меню/катсцены |
| vr.cpp | OpenXR: сессия, действия, позы, ввод → геймпад, жесты, колесо оружия, меню настроек (панель + луч), стерео-вывод, HUD-слои, прицел, шейдер блиттера, MockXR-совместимость |
| ik.cpp | IK рук по контроллерам на скелете пешки, пальцы, тело, оружие на руке, замер робота |
| weapons.cpp | список оружия, выбор, ближнее оружие (меш на кости руки), удержание пешки после удара |
| grenades.cpp | гранаты как в Alyx: взять с пояса, взвести курком, бросок инерцией руки, своя баллистика |
| pull.cpp | притягивание пикапов (как Boneworks) через FarMoveActor |
| melee.cpp | удар по касанию (выключен по умолчанию), список пешек |
| hints.cpp | подмена подсказок кнопок клавиатуры на кнопки VR (хук CreateStringNode Scaleform) |
| d3dtrace.cpp | отделение HUD от сцены (HUD рисуется в свою текстуру) |
| focus/registry/d3d9modes/inputblock/debug | окно, разрешение рендера, блок мыши, автоматизация |

### Вывод изображения (текущий)
- **Чередование глаз (AFR)**: один кадр игры = один глаз. Пара L+R отдаётся в OpenXR как projection layer.
- **Однопоточный рендер** (`[VR] SingleThreadRender=1`): патч `0x11002BB2` (`A3 44 0C B1 12 E9…` → `ret`) не даёт
  движку запустить рендер-поток (`GUseThreadedRendering` 0x12B10C44). В одном такте: frame wait → dt → Draw
  вьюпорта (из 0x1145B64A) → внутри UpdateViewTarget (мод ставит позу глаза) → рендер → Present (тот же поток).
  Показ берёт позу **своего такта** (`g_tickEye`, номер такта), без очереди. FIFO-очередь осталась для
  `SingleThreadRender=0`. FPS в MockXR ~176 (для 90 Гц нужно ~180).
- **Удержание времени** (`SyncEyeTime=1`, game.cpp OnFrameDelta): кадр правого глаза получает dt = 1 мс
  (`HeldFrameTime=0.001`), остаток отдаётся следующему левому — оба глаза показывают почти один момент. С 0.01 мс
  физика машины взрывалась (860 000 ед/с при ударе). `SyncEyeTimeVehicle=1`.
- **Правый глаз** повторяет позу скелета левого (ik.cpp g_frozen + сдвиг пешки).
- **Отзыв пользователя (НЕ решено)**: всё ещё рассинхрон — объект перед глазами (машина) «плывёт», то плоский, то
  объёмный. Это главная задача следующего этапа: изучить документацию/подходы (UEVR «synchronized sequential»,
  R.E.A.L. Luke Ross, VorpX), возможно рендер обоих глаз из одного состояния игры (`StereoPerFrame` — попытка
  была: двойной Draw не меняет вид, движок берёт вид из своей копии; в однопоточном режиме стоит попробовать заново).

### HUD
- HUD игры отделяется в текстуру; шейдер (`kBlitHLSL` в vr.cpp) режимы: 1 весь, 2 минимальный (панель: верхние углы
  + полоса подсказок), 3 зум, 4 «в глаза» (только синие и красные пиксели меток поверх картинки глаза).
- Плавающая панель HUD перед игроком (догоняет взгляд). Полноэкранные вспышки (ровная полупрозрачная заливка)
  выкидываются. LOW AMMO / «X RELOAD» (x 0.555–0.73, y 0.44–0.535 кадра) вырезаны в геймплее.
- Прицел: VR-кольцо по лучу контроллера на точке попадания; `ReticleStyle=1` — показывается вырезка центра HUD
  (игровой прицел, 0.2 ширины, круг, без синих меток). Размер — `ReticleSize` (0.6). Оптическая линза выключена
  (`Scope=0`).

### Ввод (раскладка VR в геймплее)
- Левый стик — ход (куда смотришь), правый — поворот (плавный/снэп). Правый курок — огонь, левый — точное
  прицеливание/второе действие (в машине — ускорение). A прыжок, X перезарядка/взаимодействие.
  «Вариант 2» (выбор пользователя 30.10): Y коротко — главная абилка (RB), Y удержать — вторая (LB); B коротко —
  граната/действие (B игры), **B удержать 0.35 с — колесо оружия правой руки** (оружие + «Пустая рука»; выбор
  рукой, отпустить B; в машине — смена оружия машины). Клик левого стика — трансформация (LS), правого — ничего.
  Грипы колесо НЕ открывают (раньше мешало).
- Грипы: двуручный хват (левый у цевья), гранаты (грип у пояса + курок взвести, отпустить = бросок), притягивание
  (навести свободную руку + сжать грип), в меню — перенос зон.
- Ближний бой: резкий удар рукой ВПЕРЁД (левая или пустая правая) → кнопка ближнего боя игры (RS), без касания.
- Кнопка меню: коротко — пауза; **меню + A** (или удержание 1.5 с) — режим настройки VR (см. ниже).
- Подсказки кнопок игры переписаны на кнопки VR (hints.cpp).

### Режим настройки (setup, vr.cpp)
- Плавающая панель 640×520 px (0.64×0.52 м) перед игроком, **меню** (страница 1): 34 пункта — действия
  (`@weapon` страница оружия, `@tpose` калибровка, `@camera` камера машины, `@exit`) и главные ключи ini.
  Управление: **луч правой руки + курок** (на значении: левая половина −, правая +), или левый стик + A, правый
  стик — прокрутка. Значения пишутся в ini сразу (ini перечитывается на лету).
- Страница оружия (Y): грип руки с пушкой — пушка замирает, рука переставляется (B — и поворот) → `[Weapon.<имя>]
  GunGrip`; оранжевый квадрат — реальная рука; зелёные квадраты — зоны (цевьё `Foregrip`, подсумок `BeltOffset`)
  переносятся грипом другой руки; A — сброс оружия. Пустая правая рука → `[VR] EmptyGripR`. Пушка в левой →
  `GunGripL` (функция левой пушки сейчас не предлагается в колесе).
- Т-поза: высота глаз над полом (STAGE-пространство; нет — оценка по размаху) → `UserEyeHeight`, `AutoScale=1`;
  `WorldScale = высота глаз робота (кость головы над самой нижней костью) / высота глаз игрока`.
- Камера машины «как Space Mover»: грип + движение руки двигает камеру, курок — сохранить, B — отмена.
- Выход: нажать меню; автовыход через 3 мин. Все квады проверяются на валидность позы перед xrEndFrame.

### IK (ik.cpp)
- Скелет пешки: кости `L_/R_Arm02_Shoulder_XB`, `Arm03_Elbow_XB`, `Arm04_Hand_XB`, `Weapon01_Wpn`, пальцы
  `Finger0x_*`, голова `C_Spine04_Head_XB`. Голова скрыта; торс (`C_Spine02-04`, `Lumbar02Robo`) скрыт при
  ShowBody<2; ключицы/наплечники больше не скрываются.
- Рука: ReachWrist (тянется только плечо→локоть, `LimbThickness`), пушка на предплечье/кисти: SolveGunArm (оси
  кости так, чтобы пушка легла по контроллеру; положение пушки относительно кости измеряется из L2W).
- Пальцы по сенсорам касания контроллера (FingerPoses), хват/кулак при оружии/гранате.
- **Отзыв (НЕ решено)**: плечи выглядят плохо, «не похожи на плечи персонажа», руки короткие — требует просмотра
  в шлеме или со стороны.

---

## 6. Адреса и смещения (TWFC.exe, PE timestamp 1274231782, база 0x10900000)

Глобальные: GNames `0x12B2F0F4` (TArray; имя = entry+8, wchar; FName = index<<14 | number), GWorld `0x12B10A9C`,
GEngine `[0x12B10968]` (GameViewport +0x34C, Viewport +0x140), GCurrentTime `0x12B0BAA8`, FPS cap float
`0x12A03F30`, GDeltaTime double `0x12A05540`, GUseThreadedRendering `0x12B10C44`.

Функции/хуки: после UpdateTime `0x1100956C` (удержание dt), UpdateViewTarget `0x113A8390` (thiscall, OutVT+8 =
POV; камера = POV−0x324, CameraCache +0x300, PCOwner cam+0x1C8), frame wait `0x11593600`, Draw вьюпорта игры
`0x1184FA20` (вызывается 2 раза за тик, рисует вызов из 0x1145B64A), FPerspectiveMatrix `0x10DD5530` (+ операнды
ближней плоскости `0x11451929`, `0x1145194C`), проверка рендер-потока `0x11002B60` (хвост 0x11002BB2),
FarMoveActor `0x11689630` (thiscall world, actor, &dest, bTest, bNoCheck, bAttachedMove; снять bStatic actor+0x78
бит0 и поставить 0x2000 в +0x7C на время), LineCheck `0x1167C9B0` (флаги мира 0x2286, |1 = пешки), AttachComponent
`0x1163EF00`, DetachComponent `0x1163B730`, Scaleform CreateStringNode `0x110B9520`, execSetLocation `0x10D339E0`.
UFunction vtable `0x12904110`, Func +0x68 (скриптовый TakeDamage — пустая заглушка `ret 8`, урон своей системой;
ProcessEvent не найден). Bink: `_BinkDoFrame@4` в binkw32.dll (детект роликов → виртуальный экран).

Actor: Location +0xD8, Rotation +0xE4 (int pitch,yaw,roll), Velocity +0xF0, Acceleration +0xFC, LifeSpan +0x9C,
WorldInfo +0x98, Instigator +0x94, Components +0x50/+0x54, Touching +0xB4, флаги +0x78/+0x7C, RotationRate +0x168.
WorldInfo: TimeSeconds +0x338, MaxPhysicsDeltaTime +0x4BC, PhysX scene +0x4C0 (fixed step 1/60, 5 substeps).
World: Levels +0x34 (TArray), Level Actors +0x28. Pawn: Controller +0x1D4, InvManager +0x424 (инвентарь +0x1C8,
next +0x1C8), Weapon +0x428, **Health float +0x35C**. PlayerController: Pawn +0x1C8, PlayerInput +0x428.
Weapon: обойма +0x2A0, запас +0x2A4, меш +0x208/+0x20C. PrimitiveComponent: Owner +0x34, SceneInfo +0x40,
LocalToWorld +0x50, флаги +0xF0 (HiddenGame 0x4), Translation +0x160, Scale +0x178. Attachments меша: записи по
0x30 байт (компонент, FName кости +4, loc +8, rot +0x14). UProperty: offset = (+0x30)>>20, size = ((+0x30>>12)&0xFF)*4.
Классы: TnPlayerPawn, TnAiPawn, TnAiController, TnPlayerControllerCampaign, TnAmmoCratePickupFactory,
TnHealthPickupFactory, TnWeaponPickupFactory, TnDroppedPickup*, TnProjectileGrenade*, TnWeaponFlakGrenades.

---

## 7. Настройки (twfcvr.ini, основное)

[VR]: Mode, RenderWidth/Height=1800 (квадрат, один глаз на кадр), WorldScale (по умолчанию 200 ед/м; при AutoScale
— авто), AutoScale, UserEyeHeight, UserArmSpan, FirstPersonEyeHeight=150 / EyeForward=30, VehicleCamera (0 кабина,
1 сзади), ChaseDistance/Height, VehicleEyeHeight/Forward, VehicleHeadAim, SyncEyeTime, SyncEyeTimeVehicle,
HeldFrameTime, SingleThreadRender, StereoPerFrame=0, HandIK, ShowBody, LimbThickness, LegThickness, HandScale,
WeaponScale, FingerPoses, NearClip=10, NoNearFade, MuzzleFlashScale, Reticle, ReticleSize, ReticleStyle,
Cutscenes3D=0, GunGrip, Foregrip, EmptyGripR, AbilityAnimation=0.
[Input]: AimMode=1, SnapTurnDegrees, SmoothTurnSpeed, MoveByHead, TwoHanded, TwoHandReach/Radius, GrenadeGrab,
GrenadeThrowScale=3, GrenadeGravity, GrenadeBelt, GrenadeScale=0.5, BeltOffset, MeleeGesture, MeleeSpeed=3, MeleeContact=0,
MeleeReach, MeleeHitRadius, Pull, PullRange=12, PullCone=15, PullSpeed=0, PullAutoTake=1, PullTrace, Scope=0,
GripAbilities=0, PhysicalAim. [HUD]: Minimal=1, Prompts, MarkersInEyes, ButtonHints, Distance, Width, FollowSpeed.
[Weapon.<имя>]: GunGrip, GunGripL, Foregrip, Scope. [Debug]: Automation, LogFps, FakeHmd.
dgVoodoo.conf (BinariesVR): Filtering=16 (AF); Antialiasing только appdriven (MSAA ломает отделение HUD).

---

## 8. Состояние функций

Работает (проверено MockXR; в шлеме — по отзывам пользователя частично): 1-е лицо, IK рук, прицел контроллером,
кольцо прицела по точке попадания, колесо оружия (клик стика), двуручный хват (низ по контроллеру основной руки),
пустая правая рука, гранаты из пояса, притягивание грипом (полёт в руку, затем в пешку без выталкивания; невзятое
возвращается), удар рукой вперёд, меню настроек с лучом, Т-поза и масштаб по росту, камера машины Space Mover,
машина без вылетов, ролики Bink на виртуальном экране, катсцены плоско (Cutscenes3D=0), подсказки кнопок VR,
HUD-панель + метки в глаза.

Не проверено/сомнительно: гранаты и оружие с земли как цели притягивания (имена классов не подтверждены — список
расширен на «…Pickup…»/«…Dropped…»), автоподбор оружия (X 1.2 с), урон по врагам ударом, снайперский зум.

---

## 9. Открытые задачи (последний список пользователя)

1. **VR-вывод** — рассинхрон/«плывущие» объекты перед глазами (особенно машина): сделать правильный стерео-режим
   (изучить документацию OpenXR и подходы UEVR/R.E.A.L.; рассмотреть рендер обоих глаз из одного такта в
   однопоточном режиме вместо AFR + удержания времени).
2. **Скелет/плечи** — плечи не похожи на плечи персонажа, руки выглядят короткими/неестественно.
3. Проверить в шлеме всё из 28–29.10 (см. PROGRESS «28.10 ~23:00»).
4. Если гранаты/оружие с земли всё ещё не притягиваются — найти реальные имена их классов на уровне, где они есть.

---

## 10. Что пробовали и не надо повторять

- Очередь кадров глаз: обрезать до 2, класть кадр «при отрисовке», сбрасывать при входе в 3D — всё ломало пары на
  реальном рантайме (глубина VD скачет). Решено однопоточным рендером.
- Удержание времени 0.01 мс — взрыв физики машины. Сейчас 1 мс.
- MSAA в dgVoodoo — весь HUD уходит в глаза.
- Двойной Draw вьюпорта (StereoPerFrame) при рендер-потоке — вид не меняется (движок берёт копию вида).
- Driver4VR — не годится (нет виртуального HMD без шлема).
- Вызов скриптов (ProcessEvent) не найден; скриптовый TakeDamage пустой.
- Хват пушки в левой зеркалом правой — пользователь против (нужен свой для каждого оружия, GunGripL).
- Колесо на грипе — мешает хватать; перенесено на клик стика.
- Масштаб по размаху рук — пользователь хотел «ты вписываешься в робота» → по высоте глаз.
- Дополнительная оптическая линза — убрана по просьбе.

---

## 11. Рабочий цикл одной правки

1. Создать `_vrmod\ACTIVE_SESSION.txt`. Убедиться, что игра не запущена пользователем.
2. Править `src\`, собрать `build.bat` (ошибки компилятора — в выводе).
3. `tools\run_vent.ps1` → ждать ~75 с → проверять через лог (`twfcvr.log`), `mockxr.log`, дампы bmp
   (конвертировать в png через System.Drawing и смотреть), память (python-инструменты), позы (`mockxr_pose.txt`),
   команды (`twfcvr_cmd.txt`).
4. Закрыть игру, `run_vent.ps1 -Cleanup`, удалить дампы/скриншоты, сверить сейвы, убрать тестовые ключи из ini.
5. Дописать раздел в `PROGRESS.md`, обновить инструкцию игрока, удалить `ACTIVE_SESSION.txt`.
