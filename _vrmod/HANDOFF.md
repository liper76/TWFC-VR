# VR-мод для Transformers: War for Cybertron — передача проекта

Документ для продолжения работы другой нейросетью (или человеком). Составлен 29.10.2026 по итогам ~5 дней работы.
Подробная хронология со всеми находками и проверками — `_vrmod\PROGRESS.md` (большой, читать последние разделы).
Инструкция для игрока — `VR-мод — инструкция.txt` в корне игры (английская — `VR mod - instructions.txt`;
при изменениях править обе). Страница репозитория: `README.md` (англ.) + `README.ru.md`.
Тексты мода в шлеме — двуязычные: `Tr(ru, en)` в vr.cpp, у пунктов меню поле `en`; ini `[General] Language=auto|en|ru`.

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
  "Launch VR.bat", "Launch VR (classic).bat"    те же ярлыки с английскими именами
  VR-мод — инструкция.txt   инструкция игрока (управление, ini); VR mod - instructions.txt — английская
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
  Дампы копятся сотнями: удалять `mockxr_*_*.bmp` после каждой проверки, папку целиком не листать.
- Как реальный рантайм, отвергает квады с ненормированным кватернионом (`xrEndFrame` = -39 POSE_INVALID).
- Позы и кнопки задаются файлом `mockxr\mockxr_pose.txt` (перечитывается на лету):
  ```
  head x y z yaw pitch [roll]           (roll + = наклон головы вправо)
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
  `turrets` — турели (TnMountable*) вокруг: расстояние, направление, высота; `god 1|0` — здоровье игрока держится
  (бой с боссом; не сохраняется). Турели: предпоследняя глава десептиконов, Battle on the Terrace (бой с Омегой,
  турели по краям карты).
- `tools\run_vent.ps1` — бэкап сейвов, ini Automation=1, запуск BinariesVR\TWFC.exe на MockXR, пропуск заставок,
  проход меню до тестового чекпоинта (`tools\go_vent.txt`: Campaign → Chapter Select → глава I → **Ventilation
  Shafts**, Мегатрон). `-Cleanup` — вернуть ini. `-PopupWait 60` если окно Accounts появляется поздно.
  `-Script go_vent100.txt` — тот же путь для 100%-сейва (в Solo две строки «Resume» над Chapter Select).
  Окно игры сразу уводится за экран (`tools\offscreen.ps1`) — пользователь в это время работает за ПК.
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
| camera.cpp | CameraOnUpdate (из хука UpdateViewTarget): поза головы/глаза → камера, прицел контроллером (GameSetAim), режимы машины, меню/катсцены; сценарная проходка (камера игры > 15 м от пешки-цели 3 кадра подряд, назад < 10 м) — как катсцена |
| vr.cpp | OpenXR: сессия, действия, позы, ввод → геймпад, жесты, колесо оружия, меню настроек (панель + луч), стерео-вывод, HUD-слои, прицел, шейдер блиттера, MockXR-совместимость |
| ik.cpp | IK рук по контроллерам на скелете пешки, пальцы, тело, оружие на руке, замер робота |
| weapons.cpp | список оружия, выбор, ближнее оружие (меш на кости руки), удержание пешки после удара; турели: пределы поворота, вынос начала прицела за турель, перенос трасс выстрелов (хук LineCheck) |
| grenades.cpp | гранаты как в Alyx: взять с пояса, взвести курком, бросок инерцией руки, своя баллистика |
| pull.cpp | притягивание пикапов (как Boneworks) через FarMoveActor |
| melee.cpp | удар по касанию (выключен по умолчанию), список пешек |
| hints.cpp | подмена подсказок кнопок клавиатуры на кнопки VR (хук CreateStringNode Scaleform) |
| d3dtrace.cpp | отделение HUD от сцены: вызовы HUD — в слои (глаза, панель, прицел) |
| gfx.cpp | мост к Scaleform (gfxget/gfxset/gfxcall), HUD-мувик по слоям (`_visible` клипов) |
| d3d9modes.cpp | режим 1800² в списке D3D9, устройство D3D9 (`D3D9Flush`) |
| focus/registry/d3d9modes/inputblock/debug | окно, разрешение рендера, блок мыши, автоматизация |

### Разрешение рендера (по шлему)
- Игре размер отдают хуки: registry.cpp (ScreenResolution), d3d9modes.cpp (добавочный режим), focus.cpp (окно,
  «-windowed», DPI). Сам ключ ScreenResolution в реестре не трогать.
- `[VR] RenderScale` > 0: DllMain → `VRRenderSizeStart` (vr.cpp) берёт первую строку `BinariesVR\twfcvr_headset.txt`.
  Там последний шлем: имя, рекомендуемый размер глаза (в VD — его качество) и 8 тангенсов поля. `RenderSideFor`
  считает сторону квадрата: плотность по площади × 2·max|tan| × RenderScale, не больше `MaxRenderSize` (2560). Результат
  кладётся в `g_cfg.renderWidth/Height`. Строку пишет `SaveHeadsetFov` раз за запуск, из `WaitAndBegin`, во время
  сессии. Новый шлем или новое качество VD применяются со следующего запуска. Перезагрузка ini размер не меняет.
- **OpenXR при старте НЕ опрашивать.** Экземпляр создаёт только `InitXR` в потоке рендера. Когда опрос шёл из
  DllMain в своём потоке, а размер был 3456x3456 (Quest 3, VD), игра молча закрывалась на первом 3D-кадре.
  Причина не доказана. MockXR на 3456 работал.
- Постоянный след крашей (debug.cpp `CrashTraceStart`): «exception: …» (векторный обработчик, до 12 мест),
  «exit: … called from …» (хуки ExitProcess/TerminateProcess), «mem: …» (адресное пространство). UE3 ловит
  исключения сам и выходит без WER — смотреть эти строки в twfcvr.log.

### Вывод изображения (текущий)
- **Оба глаза за один тик** (`[VR] StereoPerFrame=1` + `SingleThreadRender=1`, по умолчанию с 29.10). Кадр OpenXR
  начинается в начале тика: `appUpdateTimeAndHandleMaxTickRate` `0x110029B0` → `VRBeginTick` (xrWaitFrame,
  xrBeginFrame, ввод). Один `xrLocateViews` на тик. Хук `DrawViewport` `0x11698A90` рисует мир два раза из одного
  состояния: проход 1 — левый глаз, проход 2 — правый (своя `FSceneViewState`, поза ещё и в `pc+0xD8..0xEC`). После
  второго Present пара уходит в `xrEndFrame`. Подробно — PROGRESS «29.10 ~20:25»; окклюзия, общая для глаз, во
  втором проходе пропускается (PROGRESS «30.10 … 0x113C75A0»).
- **Однопоточный рендер** (`[VR] SingleThreadRender=1`): патч `0x11002BB2` (`A3 44 0C B1 12 E9…` → `ret`) не даёт
  движку запустить рендер-поток (`GUseThreadedRendering` 0x12B10C44). Тик, отрисовка и Present идут в одном потоке;
  глаза тика лежат в `g_tickEyes[2]`, без очереди. FIFO-очередь осталась для `SingleThreadRender=0`.
- **AFR** (`StereoPerFrame=0`, запасной: один кадр игры = один глаз): удержание времени (`SyncEyeTime=1`,
  game.cpp OnFrameDelta, правому глазу dt = `HeldFrameTime`), правый глаз повторяет позу скелета левого (ik.cpp
  g_frozen). Жалоба на «плывущие» объекты (машина то плоская, то объёмная) была на AFR — против неё и сделан двойной
  проход; в шлеме это подтвердить.
- **Откуда картинка глаз** (vr.cpp `VRFrame`): из бэкбуфера окна; если окно меньше картинки игры (2560 на экране
  высотой 2560) — из D3D9-бэкбуфера dgVoodoo, см. HUD.

### HUD
- HUD разделяется на слои через сам Scaleform, без пиксельных эвристик (gfx.cpp + d3dtrace.cpp). Хук
  GFxMovieView::Display (`0x110432C0`) рисует мувик Hud_GFX до трёх раз, каждый раз выставляя `_visible` нужных
  клипов (по имени или по глубине в таймлайне, `kClipDef`; глубина AS2 = статическая − 16384), а d3dtrace.cpp
  направляет вызовы в слой (`HudSetLayer`): kHudEyes — Canvas игры (имена) и метки мувика, поверх картинки каждого
  глаза; kHudPanel — плавающая панель (Minimal: только верхние углы — HP, способность, оружие, патроны, гранаты;
  Prompts: подсказки под прицелом); kHudReticle — прицел, следы попаданий, индикатор усиления урона. Другие мувики
  (курсор мыши) — на панель. Зум-оверлей игры (zoomScopeAttachPoint_mc) не показывается.
- dgVoodoo отдаёт D3D11-отрисовку вызова D3D9 только при следующем вызове, и последний элемент прохода уезжал в
  следующий слой (третий штрих прицела оказывался на панели). После каждого прохода `D3D9Flush()` (d3d9modes.cpp:
  Issue event query) выдаёт его сразу. Debug: `hudflush 0|1|2`, `hudsave` (слои → BinariesVR\hud_*.bmp),
  `d3dtrace N` (D3D11 и D3D9 вызовы).
- Раненым (post-process цепочка 1) и при смерти игра рисует мувик в свою текстуру (1800x1800), гоняет скан-линии и
  кладёт её на экран одним вызовом — без обработки весь HUD с прицелом попадал в глаза. Поэтому `HudSetLayer(layer,
  true)` в gfx.cpp: пока мувик рисуется, вызовы в RT игры размером с бэкбуфер тоже идут в слой, потом игре
  возвращаются её RTV. В глазах остаются только скан-линии (эффект игры). Тест: `gfxcall
  _global.NotifyDownedWarningFade 60`.
- **Бэкбуфер dgVoodoo** (d3dtrace.cpp `NoteCopy`): текстура D3D9-бэкбуфера — источник первой копии кадра
  (CopyResource или CopySubresourceRegion) из тех текстур, в которые в этом кадре рисовали. Если рендер выше
  рабочего стола (2560 при экране высотой 2560 физ. пикселей), dgVoodoo сам ужимает окно (SetWindowPos 2566x2582 из
  Reset игры, `KeepWindowAspectRatio`) и делает swapchain 2560x2547. В окно идёт только верх картинки:
  CopySubresourceRegion и отрисовка. Раньше бралась первая CopyResource — текстура, куда игра не рисует, и HUD
  оставался в глазах. Тогда же глаза, прицел и театр берут картинку прямо из бэкбуфера игры (`TraceEndFrame` →
  `VRFrame`, лог «vr: picture from the game's backbuffer …»), целиком и квадратной. При 1800 — из swapchain окна, как
  раньше. С размером окна не воевать: dgVoodoo считает его сам, на мониторах ниже обрезка была бы больше.
- Плавающая панель перед игроком (догоняет взгляд).
- Прицел: VR-кольцо по лучу контроллера на точке попадания; `ReticleStyle=1` — вырезка центра слоя прицела
  (`kReticleCut` = 0.6 ширины, круг) на кольце в угловом размере игры. Размер — `ReticleSize` (0.6). Оптическая
  линза выключена (`Scope=0`).

### Ввод (раскладка VR в геймплее)
- Левый стик — ход (куда смотришь), правый — поворот (плавный/снэп). Правый курок — огонь, левый — точное
  прицеливание/второе действие (в машине — ускорение). A прыжок, X перезарядка/взаимодействие.
  **Клик правого стика — вторая абилка (LB)**, пока держишь. **B коротко — главная (RB**, импульс
  при отпускании; в машине — спецманёвр), **B держать 0.35 с — колесо оружия правой руки** (оружие + «Пустая рука»; выбор рукой, отпустить
  B); в машине удержание B — следующее оружие машины. **Y — B игры** (бросок гранаты; гранаты и с пояса). Правый стик —
  только поворот (вертикаль игре не идёт). «Пустая рука» на турели — X (слезть). Клик левого стика — трансформация
  (LS). (До 03.11: Y = RB, правый стик вверх = LB, B коротко = граната, правый стик вниз = следующее оружие;
  переставлено по просьбе, `StickVertical`/`WheelQuickSwitch` удалены.) Логика B — в `UpdateWeaponWheel`. Подсказки
  кнопок — hints.cpp `kKeys`. Грипы колесо НЕ открывают. LogFps=1 пишет в лог «vr: abilities LB/RB»,
  «vr: B tapped -> main ability», «vr: B held -> weapon wheel».
- Грипы: двуручный хват (левый у цевья; виртуальный приклад `Gunstock` 0/1 к плечу/2 у цевья и сглаживание
  `WeaponSmoothing` 0..10 — `GunPose` в vr.cpp, применяется к рукам и лучу выстрела), гранаты (грип у пояса + курок взвести, отпустить = бросок), притягивание
  (навести свободную руку + сжать грип), в меню — перенос зон.
- Турель: при посадке перед игроком две ручки и перекладина (квады, vr.cpp `BuildTurretGrips`). Сжать грип у
  ручки — взять (одной или двумя руками), поворот рук вокруг головы поворачивает пушку (в пределах турели),
  курки как с пушкой: правый (рука с пушкой, G) — огонь, левый — LT игры (прицел), и при обеих руках на ручках
  (раньше курок любой держащей руки стрелял, и при двух руках прицела не было). X — слезть, B держать ~4 с —
  оторвать турель (переносная, TnWeaponGatlingGun). На турели наплечники и всё, что висит на ключицах кроме руки,
  сжаты до 0.01 (ik.cpp `shoulderGear`, только при `WeaponsOnTurret()`; в лог «ik: shoulder part …»).
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
  переносятся грипом другой руки; A — сброс оружия (этого персонажа). Настройки — по оружию И персонажу:
  `[Weapon.<имя>@<персонаж>]` с `Own=1` (персонаж = `IKCharacter()`, напр. `RB_MegatronWeaponArm_SKEL`), нет его —
  общий `[Weapon.<имя>]` (последняя настройка оружия на ком угодно). Значения оружия живут только в `g_ws`
  (`g_cfg` — глобальные; раньше копировались в g_cfg и перезапись ini их сбрасывала). Каждое изменение пишется
  сразу в оба раздела (SaveSetup), не только при выходе. Пустая правая рука → `[VR] EmptyGripR`. Пушка в левой →
  `GunGripL` (функция левой пушки сейчас не предлагается в колесе).
- Т-поза: высота глаз над полом (STAGE-пространство; нет — оценка по размаху) → `UserEyeHeight`, `AutoScale=1`;
  `WorldScale = высота глаз робота (кость головы над самой нижней костью) / высота глаз игрока`.
- Камера машины «как Space Mover»: грип + движение руки двигает камеру, курок — сохранить, B — отмена.
- Выход: нажать меню; автовыход через 3 мин. Все квады проверяются на валидность позы перед xrEndFrame.

### IK (ik.cpp)
- Скелет пешки: кости `L_/R_Arm02_Shoulder_XB`, `Arm03_Elbow_XB`, `Arm04_Hand_XB`, `Weapon01_Wpn`, пальцы
  `Finger0x_*`, голова `C_Spine04_Head_XB`. Голова скрыта; торс (`C_Spine02-04`, `Lumbar02Robo`) скрыт при
  ShowBody<2; ключицы/наплечники больше не скрываются.
- Рука: SolveArm — кисть всегда в контроллере: тянется плечо→локоть, а вне досягаемости и предплечье (до
  `kMaxReach` = 1.4). `ArmSize` (0.8, меню «Размер рук») утоньшает всю руку поперёк кости — плечо, предплечье,
  броню (`g_padFrame`) и модель руки на пустой кисти; `LimbThickness` — плечо ещё сильнее. Рука с пушкой —
  SolveGunArm, как в игре: предплечье от локтя = пушка. Оси пушки = контроллер (+GunGrip),
  предплечье жёстко к пушке по родной связи (`handInGun` из вложения игры), кисть-кулак на рукояти; локоть = запястье
  − предплечье × WeaponScale (пушка рисуется в WeaponScale вокруг кисти), плечо тянется 0.6..`kGunReach` (3.0), броня
  локтя в размере пушки. `armdump` пишет «armdump aim: upper arm … wanted … got» — дотянулось ли плечо до локтя. Модель руки игры (бросок гранаты) под пушкой скрыта.
  Пушку, которую игра спрятала сама (HiddenGame, флаги +0xF0 бит 2: кнопка, рычаг — интеракция), мод считает
  отсутствующей: `g_gun.held` = false → правая рука пустая, видна модель пустой руки (раньше рука стояла на
  невидимой пушке и модель руки была скрыта — «невидимая рука»). Лог: «ik: the game hid the gun - the hand empty». HandScale/WeaponScale
  по умолчанию 1.0 (0.6 делали пушку меньше родной — «рука в пушке» выглядела плохо). Debug `armdump` пишет ещё строки
  «armdump aim» (начало пушки и локоть в осях прицела, пальцы, родные оси пушки).
- Пустая правая рука: модель руки игры (CP_<робот>Arm_SKEL) в позе привязки, повёрнута на пол-оборота вокруг y и
  сдвинута к кисти на удлинение предплечья (PlaceArmModel, `g_foreExtra`). Кладётся по кадру локтя сам, НЕ по
  креплению игры: после взятия пушки игра крепит модель с `rot 32768 0 0` (пол-оборота вокруг y), и с ним кожух
  стоял задом наперёд позади кисти. Debug `armfix <0 z|1 y> <вперёд> <y> <z>`
  меняет поворот и добавочный сдвиг на лету, `armbind 0` — пластины как их ставит игра.
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
`DrawViewport(FViewport*)` `0x11698A90` (__stdcall, ret 4; из UGameEngine::RedrawViewports `0x11698DDD` и загрузочного
`0x115CB48F`; `0x1184FA20` — это UAudioDevice::Update, не отрисовка), начало тика
`appUpdateTimeAndHandleMaxTickRate` `0x110029B0`, FPerspectiveMatrix `0x10DD5530` (+ операнды
ближней плоскости `0x11451929`, `0x1145194C`), проверка рендер-потока `0x11002B60` (хвост 0x11002BB2),
FarMoveActor `0x11689630` (thiscall world, actor, &dest, bTest, bNoCheck, bAttachedMove; снять bStatic actor+0x78
бит0 и поставить 0x2000 в +0x7C на время), LineCheck `0x1167C9B0` (thiscall world, FCheckResult* hit (actor +4, location +8), source, -, &end, &start,
flags, &extent, -, -; флаги мира 0x2286, |1 = пешки; выстрелы игрока мгновенного оружия — флаги 0x20bf/0x228bf от
глаз пешки длиной 30000, трасса прицела 0x28bf; зовёт execTrace `0x114BE9D0` → `0x114BE8E0`; execTraceActors
`0x114B6620`, execFastTrace `0x114B6C10`), AttachComponent
`0x1163EF00`, DetachComponent `0x1163B730`, Scaleform CreateStringNode `0x110B9520`, execSetLocation `0x10D339E0`.
UFunction vtable `0x12904110`, Func +0x68 (скриптовый TakeDamage — пустая заглушка `ret 8`, урон своей системой;
ProcessEvent не найден). Bink: `_BinkDoFrame@4` в binkw32.dll (детект роликов → виртуальный экран).

Actor: Location +0xD8, Rotation +0xE4 (int pitch,yaw,roll), Velocity +0xF0, Acceleration +0xFC, LifeSpan +0x9C,
WorldInfo +0x98, Instigator +0x94, Components +0x50/+0x54, Touching +0xB4, флаги +0x78/+0x7C, RotationRate +0x168.
WorldInfo: TimeSeconds +0x338, MaxPhysicsDeltaTime +0x4BC, PhysX scene +0x4C0 (fixed step 1/60, 5 substeps).
World: Levels +0x34 (TArray), Level Actors +0x28. Pawn: Controller +0x1D4, InvManager +0x424 (инвентарь +0x1C8,
next +0x1C8), Weapon +0x428, **Health float +0x35C**, FlashLocation +0x42C (точка последнего попадания). PlayerController: Pawn +0x1C8, PlayerInput +0x428.
Weapon: обойма +0x2A0, запас +0x2A4, меш +0x208/+0x20C. PrimitiveComponent: Owner +0x34, SceneInfo +0x40,
LocalToWorld +0x50, флаги +0xF0 (HiddenGame 0x4), Translation +0x160, Scale +0x178. Attachments меша: записи по
0x30 байт (компонент, FName кости +4, loc +8, rot +0x14). UProperty: offset = (+0x30)>>20, size = ((+0x30>>12)&0xFF)*4.
Турель (TnMountable*): пределы поворота +0x2B0 (рыскание −90..90 от базы, тангаж −30..25), меш пушки — оружие
+0x20C/+0x208; SkeletalMeshComponent SpaceBases +0x1EC (матрицы 64 байта, перенос +48), Bounds +0xBC.
Scaleform: GGFxEngine `[0x12B000B4]`, +0x40 TArray FGFxMovie*; FGFxMovie +0x34 GFxMovieView, +0x40 видим, +0x50
UGFxMovie («Hud_GFX»). Vtable вида: +0x2C SetVariable(path, GFxValue&, 1), +0x38 GetVariable(GFxValue* out, path),
+0x60 Invoke(путь метода, GFxValue* result, args (шаг 0x20), n), +0x90 Advance, +0x94 Display (2 раза за тик);
GFxValue: тип +0 (2 bool, 3 number, 4 string, 6 object, 7 array, 8 display object), значение +8. Только из игрового потока.
Клипы из GetVariable приходят строкой-путём. HUD: прицел `_root.hud_mc.midCenter_mc.instance17.crosshairAnchor_mc`
(клип «crosshair»N«_mc» через attachMovie), менеджер меток `_root.instance24` (в кампании пуст: имена над
напарниками и синие метки рисует не он) (markerArray, клипы «marker»N«_mc»,
screenCenterX/Y = 900, Stage 1800 = картинка 1800², _x/_y метки — пиксели экрана). Debug-команды: `gfxget`,
`gfxset`, `gfxcall [movie:]метод арг…`, `gfxkids <клип> [n]` (дети клипа по глубинам), `gfxprobe` (src\gfx.cpp).
Группа прицела `instance17` сама добавляет клипы на глубины от 0 (`causeDamageN_mc` — хитмаркер,
`turretReticuleN_mc`, `rangeFinderN_mc`): gfx.cpp прячет их с панели. AS мувиков читается
`_vrmod_tools\as2dis.py movie.gfx [слова]` (дизассемблер AS2 с псевдокодом).
LevelView и HUD глаз: хук Canvas Project (0x115D54E0, game.cpp) раздвигает точки меток в VRHudProjectScale() раз,
vr.cpp накладывает слой HUD глаз уменьшенным в столько же раз (размер меток не зависит от крена).
Мувики HUD, снятые из памяти (*.gfx), и разбор их дерева клипов (hud_tree.txt, hud_strings.txt) лежат в
`_vrmod_tools\gfxdump` (не в git: это ресурсы игры).
Классы: TnPlayerPawn, TnAiPawn, TnAiController, TnPlayerControllerCampaign, TnAmmoCratePickupFactory,
TnHealthPickupFactory, TnWeaponPickupFactory, TnDroppedPickup*, TnProjectileGrenade*, TnWeaponFlakGrenades.

---

## 7. Настройки (twfcvr.ini, основное)

[VR]: Mode, RenderScale=1.0 (>0: размер рендера по шлему × это, при старте игры; 0 = всегда RenderWidth/Height),
MaxRenderSize=2560 (предел для RenderScale; 0 = без предела),
RenderWidth/Height=1800 (квадрат, один глаз на проход; пока ни один шлем не встречался), WorldScale (по умолчанию 200 ед/м; при AutoScale
— авто), AutoScale, UserEyeHeight, UserArmSpan, FirstPersonEyeHeight=150 / EyeForward=30, VehicleCamera (0 кабина,
1 сзади), ChaseDistance/Height, VehicleEyeHeight/Forward, VehicleHeadAim, SyncEyeTime, SyncEyeTimeVehicle,
HeldFrameTime, SingleThreadRender=1, StereoPerFrame=1 (оба глаза за тик; 0 = AFR), HandIK, ShowBody, ArmSize=0.8, LimbThickness, LegThickness, HandScale,
WeaponScale, FingerPoses, NearClip=10, NoNearFade, MuzzleFlashScale, Reticle, ReticleSize, ReticleStyle,
Cutscenes3D=0, GunGrip, Foregrip, EmptyGripR, AbilityAnimation=0, LevelView=1 (глаза рисуются без крена головы, рантайм
доворачивает: имена и метки ровно по миру; фрустум растёт на крен).
[Input]: AimMode=1, SnapTurnDegrees, SmoothTurnSpeed, MoveByHead, TwoHanded, TwoHandReach/Radius, Gunstock=2, WeaponSmoothing=3,
(StockShoulder/StockButt/StockSight), GrenadeGrab,
GrenadeThrowScale=3, GrenadeGravity, GrenadeBelt, GrenadeScale=0.5, BeltOffset, MeleeGesture, MeleeSpeed=3, MeleeContact=0,
MeleeReach, MeleeHitRadius, Pull, PullRange=12, PullCone=15, PullSpeed=0, PullAutoTake=1, PullTrace, Scope=0,
GripAbilities=0, PhysicalAim. [HUD]: Minimal=1, Prompts, MarkersInEyes, ButtonHints, Distance, Width, FollowSpeed.
[Weapon.<имя>]: GunGrip, GunGripL, Foregrip, Scope, Stock (0 = без приклада; по умолчанию нет у «Pistol»). [Debug]: Automation, LogFps, FakeHmd.
dgVoodoo.conf (BinariesVR): Filtering=16 (AF); Antialiasing только appdriven (MSAA ломает отделение HUD).

---

## 8. Состояние функций

Работает (проверено MockXR; в шлеме — по отзывам пользователя частично): 1-е лицо, IK рук, прицел контроллером,
кольцо прицела по точке попадания, колесо оружия (удержание B), двуручный хват (низ по контроллеру основной руки; приклад и сглаживание — только MockXR),
пустая правая рука, гранаты из пояса, притягивание грипом (полёт в руку, затем в пешку без выталкивания; невзятое
возвращается), удар рукой вперёд, меню настроек с лучом, Т-поза и масштаб по росту, камера машины Space Mover,
машина без вылетов, ролики Bink на виртуальном экране, катсцены плоско (Cutscenes3D=0), подсказки кнопок VR,
HUD-панель + метки в глаза, HUD при малом HP (только MockXR), колесо со всем оружием инвентаря (Мегатрон: Bazooka
и Rocket Launcher, MockXR).

Не проверено/сомнительно: гранаты и оружие с земли как цели притягивания (имена классов не подтверждены — список
расширен на «…Pickup…»/«…Dropped…»), автоподбор оружия (X 1.2 с), урон по врагам ударом, снайперский зум; от
02.11: позиция оружия при смене (по оружию в руке, `WeaponList::held`), ручки турели в стиле игры, точка подбора,
временные ElbowInGun*, разрешение по шлему (RenderScale; из файла шлема, до 2560 — в шлеме не проверено после
краша на 3456), кожух пустой руки после смены оружия — в шлеме не проверены. HUD на панели и полная квадратная
картинка при 2560 (окно 2560x2547) — только MockXR.

---

## 9. Открытые задачи (последний список пользователя)

1. **VR-вывод** — сделан режим «оба глаза за тик» (29.10, `StereoPerFrame=1`). Проверить в шлеме, ушли ли
   рассинхрон и «плывущие» объекты перед глазами (особенно машина), и хватает ли GPU на два прохода.
2. **Скелет/плечи** — плечи не похожи на плечи персонажа, руки выглядят короткими/неестественно.
3. Проверить в шлеме всё из 28–29.10 (см. PROGRESS «28.10 ~23:00»).
4. Если гранаты/оружие с земли всё ещё не притягиваются — найти реальные имена их классов на уровне, где они есть.
5. **ВРЕМЕННЫЕ настройки `[Input] ElbowInGunForward/Up/Out`** (меню: «ВРЕМЕННО: локоть в оружии — …», ik.cpp
   `SolveArm`, main.cpp, vr.cpp, `g_cfg.elbowInGun`) — пользователь калибрует, куда локоть входит в оружие. Когда
   выставит — перенести его значения из twfcvr.ini в код как константы и убрать пункты и ключи.
6. Проверить в шлеме от 02.11: старт после краша на первом 3D-кадре (лог «vr: render size 2560x2560 for Meta Quest 3 …»;
   при выходе — строки «exception:»/«exit:»/«mem:»; если снова закрывается — MaxRenderSize ниже или RenderScale=0),
   кожух пустой правой руки после смены оружия (Мегатрон и другие роботы), вид ручек турели, точку подбора,
   сохранение позиции оружия при смене. При 2560: HUD только на панели, а не в глазах; картинка не растянута;
   в логе «vr: picture from the game's backbuffer 2560x2560 (the window's swapchain is …)».

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
- Колесо на грипе — мешает хватать; потом было на клике стика и на удержании B; с 31.10 — правый стик вниз.
- Абилки на Y (левый большой палец) и клике стика — неудобно, палец занят ходьбой; теперь B и правый стик вверх.
- Масштаб по размаху рук — пользователь хотел «ты вписываешься в робота» → по высоте глаз.
- Дополнительная оптическая линза — убрана по просьбе.
- Прицеливание турелью «рычагом» (ось 0,30 м впереди ручек: поднял руки — ствол вниз) — пользователь вернул
  прежнее (наклон от высоты рук).

---

## 11. Рабочий цикл одной правки

1. Создать `_vrmod\ACTIVE_SESSION.txt`. Убедиться, что игра не запущена пользователем.
2. Править `src\`, собрать `build.bat` (ошибки компилятора — в выводе).
3. `tools\run_vent.ps1` → ждать ~75 с → проверять через лог (`twfcvr.log`), `mockxr.log`, дампы bmp
   (конвертировать в png через System.Drawing и смотреть), память (python-инструменты), позы (`mockxr_pose.txt`),
   команды (`twfcvr_cmd.txt`).
4. Закрыть игру, `run_vent.ps1 -Cleanup`, удалить дампы/скриншоты, сверить сейвы, убрать тестовые ключи из ini.
5. Дописать раздел в `PROGRESS.md`, обновить инструкцию игрока, удалить `ACTIVE_SESSION.txt`.

- **Сценарная проходка (Autobot, глава VII «Prison Break», начало).**
  - Цель вида остаётся пешкой игрока, а камера игры идёт за анимированными роботами. Пешка в это время стоит в
    темноте далеко (до 21000 uu от камеры).
  - Раньше глаза ставились в пешку, и игрок стоял в темноте рядом с напарниками.
  - Теперь, если камера игры дальше 1500 uu от пешки 3 кадра подряд, вид показывается как катсцена (Cutscenes3D).
    Обратно к пешке — когда камера ближе 1000 uu. Лог: «camera: scripted view far from the pawn…» / «camera: back
    to the pawn».
  - Дойти: `run_vent.ps1 -PopupWait 62 -Script go_prison.txt` (100%-сейв; глава VII, первый чекпоинт, Оптимус).
