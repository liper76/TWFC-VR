# TWFC-VR

[English](README.md) | **Русский**

VR-мод для **Transformers: War for Cybertron** (2010, PC, Unreal Engine 3, 32-bit, DirectX 9).
Работает через OpenXR (проверяется с Virtual Desktop и контроллерами Quest).

- вид от первого лица, стерео (оба глаза за один тик игры), трекинг головы;
- руки робота по контроллерам (IK), прицеливание контроллером, двуручный хват;
- колесо оружия, гранаты с пояса, притягивание предметов, удар жестом;
- HUD на плавающей панели, подсказки кнопок VR, меню настроек в шлеме;
- ролики и меню — на виртуальном экране;
- интерфейс мода на русском и английском (переключатель — первый пункт меню настроек VR).

## Язык

Меню настроек VR и колесо оружия бывают на русском и английском. По умолчанию язык берётся из Windows
(русская, украинская или белорусская Windows — русский, иначе английский). Переключить: в шлеме откройте
меню настроек (кнопка меню + A) — первый пункт **Language / Язык**, или в `BinariesVR\twfcvr.ini`:

```
[General]
Language=ru        ; auto | en | ru
```

Инструкция игрока — на двух языках: `VR-мод — инструкция.txt` и `VR mod - instructions.txt`.

## Как это устроено

Мод — прокси-библиотека `xinput1_3.dll`, которую загружает игра. Она патчит движок (MinHook), отделяет HUD,
забирает кадры из D3D11 (игра рендерится через dgVoodoo: D3D9 → D3D11) и отдаёт их в OpenXR.
Подробности — [_vrmod/HANDOFF.md](_vrmod/HANDOFF.md), журнал работы — [_vrmod/PROGRESS.md](_vrmod/PROGRESS.md).

Репозиторий лежит прямо в папке игры, но в нём только файлы мода (см. `.gitignore`):

```
README.md / README.ru.md     страница проекта (английская / русская)
VR-мод — инструкция.txt      инструкция игрока (управление, настройки)
VR mod - instructions.txt    то же на английском
Запуск VR.bat                запуск игры с модом (то же — Launch VR.bat)
Запуск VR (классика).bat     запуск старой версии, 3-е лицо (то же — Launch VR (classic).bat)
BinariesVR/dgVoodoo.conf     настройки dgVoodoo для мода
_vrmod/src/                  исходники мода (C++)
_vrmod/build.bat             сборка и установка в BinariesVR
_vrmod/tools/                инструменты анализа (python), MockXR (фейковый OpenXR-рантайм), скрипты тестов
_vrmod/snapshots/            снимок исходников классической версии
```

## Сборка

Нужны: VS 2022 Build Tools (x86), в `_vrmod_tools\` рядом с `_vrmod\` — MinHook 1.3.4 (`minhook\minhook-1.3.4`)
и OpenXR SDK (`openxr\include`, `openxr\Win32\lib|bin`).

```
cmd /c "_vrmod\build.bat"
```

Собирает `xinput1_3.dll` и копирует её вместе с `openxr_loader.dll` в `BinariesVR\`.

## Установка

`BinariesVR\` — копия `Binaries\` игры, в которую добавлены `xinput1_3.dll` (мод), `openxr_loader.dll`
и dgVoodoo 2 (`D3D9.dll` и др. + `dgVoodoo.conf` из репозитория). Оригинальная папка `Binaries\` не меняется.
Настройки мода — `BinariesVR\twfcvr.ini` (создаётся при первом запуске), лог — `BinariesVR\twfcvr.log`.

В репозиторий не входят файлы игры, dgVoodoo, MinHook, OpenXR SDK и собранные бинарники.
