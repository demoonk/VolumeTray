# VolumeTray

Небольшая утилита для Windows (только консольного окна нет — скрытое окно + иконка в трее).

## Что делает

- **Прокрутка колеса мыши над панелью задач** — меняет громкость текущего устройства вывода по умолчанию (Bluetooth / USB Audio / HDMI монитора). Работает и на вторичных панелях задач (`Shell_SecondaryTrayWnd`); панель определяется по классу окна либо по полосе между рабочей областью и краем монитора.
- **Средняя кнопка мыши над пустым местом панели задач** — mute (только если курсор именно на панели, а не на кнопке/трей-области).
- **Под полноэкранным приложением** — работает, если включён полноэкранный режим (по умолчанию включён). Окна shell (`Progman`, `WorkerW`, `Shell_TrayWnd`, `CoreWindow` и т.п.) и maximized-окна (`IsZoomed`) полноэкранными не считаются.
- Громкость меняется через `SendInput` системными клавишами `VK_VOLUME_UP` / `VK_VOLUME_DOWN` / `VK_VOLUME_MUTE`, поэтому Windows сама показывает свой OSD-индикатор и применяет изменение к устройству, выбранному в системе по умолчанию. Какого-либо собственного миксера или API громкости в коде нет.
- Ловится и горизонтальное колесо (`MS_HWHEEL`): направление зависит от знака `mouseData`.
- Если событие обработала громкость, оно **гасится** (hook возвращает `1`, `CallNextHookEx` не вызывается) — приложение под курсором его не получает. Без этого YouTube в полноэкранном при прокрутке над таскбаром ещё и скролит страницу и выкидывает ряд плиток. Выключается галочкой «Гасить колесо» (`Swallow`).

## Трей-меню

Открывается по правой кнопке, контекстному меню или левой кнопке на иконке:

- **Работать в полноэкранном режиме** — переключает `Fullscreen`.
- **Средняя кнопка на таскбаре — Mute** — переключает `MiddleMute`.
- **Скорость** — подменю `-6 … +6`, переключает `Speed` (галочка на текущем значении).
- **Реверс колеса** — переключает `Reverse` (галочка).
- **Гасить колесо (не передавать системе)** — переключает `Swallow` (галочка).
- **Запускать при старте Windows** — значение `VolumeTray` в `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Выход** — убирает иконку и завершает процесс.

Пункты меню показывают текущее состояние галочками. После пересоздания панели задач (`TaskbarCreated`) иконка добавляется заново.

## Настройки

INI-файл рядом с exe (`VolumeTray.ini`, путь строится из пути exe):

```ini
[Settings]
Fullscreen=1
MiddleMute=1
Speed=0
Reverse=0
Swallow=1
Debug=1
```

Значения читаются при старте и сохраняются при переключении в меню. Значение по умолчанию для `Fullscreen`, `MiddleMute` и `Swallow` — `1`, для `Speed` и `Reverse` — `0`. `Debug=1` — писать каждое событие мыши в `%APPDATA%\VolumeTray.log` (точка, прошла ли проверка таскбара, результат `SendInput`, гасится ли событие), по умолчанию `0`.

`Speed` — число нажатий клавиши на одно движение колеса, от `-6` до `+6`. `0` — это **один шаг** (значения 0 быть «ничего не отправлять» не может, поэтому `n == 0 → n = 1`); знак `Speed` инвертирует направление. `Reverse=1` — галочка «Реверс колеса» в меню, меняет направление независимо от знака `Speed`; два знака складываются через XOR (`WM_TRAYWORK`, [main.cpp:526](main.cpp#L526>)).

`Swallow=1`: решение о том, гасить ли событие, принимается **в hook** (там только read-only проверки — `MonitorFromPoint`/`GetMonitorInfo`/`WindowFromPoint`/`GetClassName`/`GetForegroundWindow`, никаких сообщений другим процессам), а отправка клавиш остаётся в message loop. Компромисс: событие гасится до отправки, поэтому если `SendInput` откажет (UIPI, Low IL), прокрутка до приложения уже не дойдёт — при сомнении снимите галочку.

## Видимость иконки в трее (Windows 11)

На Windows 11 (24H2+) иконка нового приложения по умолчанию скрыта: она только в overflow-флайоте (^), а per-app тумблер «Other system tray icons» для новых приложений выключен. Включить: **Settings → Personalization → Taskbar → Other system tray icons → VolumeTray → On** (или через `HKCU\Control Panel\NotifyIconSettings` — ключ с `ExecutablePath=...\VolumeTray.exe`, `IsPromoted=1`, после этого перезапустить Explorer). Сам процесс иконку «продвинуть» не может — это только настройка пользователя.

## Иконка в трее

Итоговая иконка — [icons/make_icon_final.py](icons\make_icon_final.py>) (только Pillow), результат — `icons/app.ico`, записи **16, 20, 24, 32, 40, 48, 64, 128, 256**, все PNG `colortype=6` (RGBA, альфа сохранена).
Шаги: маска глифа из референса `icons/ref_new.png` (460×460, чёрный контур на белом) → заполнение контура (flood fill от края: что не достижимо с края — внутренность глифа, иначе на 16 px линии в 1 px не читаются) → цвет глифа `GLYPH (198,154,240)` (светло-фиолетовый), фон — прозрачный → crop по альфе (bbox глифа 451×397 → квадрат 1:1 с равным полем 8 px) → LANCZOS по всем размерам. Превью: `icons/check_tray<N>.png`, сетка — [icons/check_montage_final.png](icons\check_montage_final.png>), сравнение контур/заполненный/квадрат — [icons/check_strip_ABC.png](icons\check_strip_ABC.png>).

Все размеры обязательны: shell сам выбирает запись по DPI. Если в `.ico` была бы одна большая запись, Windows масштабировала бы её вниз — отсюда эффект «странной» (размытой/срезанной) иконки.

**Откуда Windows берёт такой глиф:** перебраны `imageres.dll` (785 иконок), `shell32.dll` и `mmres.dll` (18) — совпадения нет, лучший IoU к силуэту 0.46 (это просто похожие по площади пятна, не динамик). Значит приложенный референс — Fluent-глиф Windows 11, который XAML-таскбар рисует из SVG во время отрисовки; как `.ico`-ресурс с альфой в системе он не лежит, поэтому иконку пришлось перерисовать.

Два бага, из-за которых иконка выглядела чужой, исправлены:

1. **Имя ресурса должно быть числом.** В [app.rc](app.rc>) написано `101 ICON "icons\\app.ico"`, а не `IDI_APPICON ICON ...`. `windres` хранит имя ресурса как UTF-16LE строку: символьное имя сохраняется под именем `"IDI_APPICON"`, и `LoadIconW(hInst, MAKEINTRESOURCE(101))` его не находит → приложение падает на системный fallback `IDI_APPLICATION`.
2. **Поле `count` в `ICONGROUPHEADER`.** Структура: `DWORD reserved, WORD count, WORD type` — `count` на offset **4**, а не 2. Проверено через `FindResourceExW`: `count@2 = 1`, `count@4 = 9`.

Проверка (без ручного парсинга PE — только Win32 API):

```text
FindResourceEx(14,101) err=0
group count=9   16 20 24 32 40 48 64 128 256
```

## Почему «работает только от админа»

Это **не** требование привилегий самим кодом — это UIPI. Процесс с низким уровнем целостности (Low IL, `S-1-16-4096`) не может обратиться к Explorer (Medium IL, `S-1-16-8192`), поэтому `Shell_NotifyIconW(NIM_ADD)` возвращает `err=5` (`ERROR_ACCESS_DENIED`). Иконка физически не может появиться без повышения.

Поэтому в коде:

- при старте логируется `started IL=<уровень> elevated=<0/1>`;
- если процесс не повышен — запрашивается повышение через `ShellExecuteW(NULL, L"runas", ...)` (документированный verb; UAC-промпт появляется и это ожидаемо) и текущий экземпляр выходит. **Если пользователь откажет в повышении, код продолжает работать без него**: хук и колесо работают, но иконка не появится — об этом сообщает окно с объяснением;
- при `NIM_ADD err=5` показывается сообщение с объяснением причины, а не молчаливый отказ;
- «Запускать при старте Windows»: если процесс уже повышен — пишется прямой путь; иначе обёртка `rundll32 shell32,ShellExecByPath -runas "<exe>"`, которая запрашивает повышение при логоне.

Уровни целостности (`S-1-16-*`): 0 Untrusted, 4096 Low, 8192 Medium, 12288 High, 16384 System. `IsProcessElevated()` проверяет sub-authority 544 (`S-1-5-32-544`) в `TokenUser`, `TokenElevationType == TokenElevationTypeFull` и `TokenGroups` (пропуская `SE_GROUP_USE_FOR_DENY_ONLY`).

## Сборка

w64devkit. Иконка берётся из ресурса: `app.rc` → `101 ICON "icons\\app.ico"` (файл `icons\app.ico` должен существовать; в `.rc` путь — C-строка, поэтому разделитель пишется как `\\`: в `icons\app.ico` последовательность `\a` превращается в BEL и windres не открывает файл). Типичная команда сборки:

```bat
windres app.rc -O coff -o app.res
g++ -O2 -s -mwindows -municode -DUNICODE -D_UNICODE main.cpp app.res -o VolumeTray.exe -lshell32 -luser32 -lgdi32 -ladvapi32
```

`-mwindows` нужен, чтобы не было консоли; `-municode` и `-DUNICODE -D_UNICODE` обязательны для MinGW (`wWinMain` и `MAKEINTRESOURCE`); `shell32` — для `Shell_NotifyIconW`. Готовый скрипт — [build.bat](build.bat>) (нужен w64devkit или MinGW-w64 на PATH; собирает в `build\VolumeTray.exe`).

## Одиночный экземпляр

При старте создаётся мьютекс `Local\VolumeTray.singleinstance` (per-session, без привилегий). Если он уже существует — второй экземпляр показывает сообщение «VolumeTray уже запущен» и выходит.

## Что сверено с Microsoft Learn перед кодом

| Решение в коде | Источник (Learn) |
| --- | --- |
| Если hook возвращает ненулевое значение и **не** вызывает `CallNextHookEx`, событие не передаётся дальше по цепочке — этим гасится прокрутка, когда её обработала громкость (`Swallow=1`, решение принимается в `VolumeWillHandle`) | [LowLevelMouseProc → Return value](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelmouseproc) |
| `hMod = NULL` для `WH_MOUSE_LL`, hook живёт в потоке установщика, нужен message loop, снятие через `UnhookWindowsHookEx` | [SetWindowsHookExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowshookexw) |
| Hook обязан возвращаться за единицы мс; при превышении `LowLevelHooksTimeout` (`HKCU\Control Panel\Desktop`, максимум 1000 мс с 1709) hook **тихо снимается без вызова** — отсюда watchdog `WM_TIMER` + переустановка | [LowLevelMouseProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelmouseproc), [Hooks Overview](https://learn.microsoft.com/en-us/windows/win32/winmsg/about-hooks) |
| `pt` — пер-мониторные экранные координаты; `mouseData` (старшее слово) = delta колеса, `WHEEL_DELTA` = 120, знак — направление; `LLMHF_INJECTED` / `LLMHF_LOWER_IL_INJECTED` | [MSLLHOOKSTRUCT](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-msllhookstruct), [WM_MOUSEWHEEL](https://learn.microsoft.com/en-us/windows/win32/inputdev/wm-mousewheel) (горизонтальное колесо — `MS_HWHEEL`, тот же delta) |
| `SendInput`: `cbSize == sizeof(INPUT)`; UIPI — инъекция разрешена только в процессы с равным или меньшим IL, и отказ **не отражается** в возвращаемом значении/`GetLastError` | [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) |
| `cbSize == sizeof(NOTIFYICONDATAW)`, `version = 0` при первом `NIM_ADD`, затем `NIM_SETVERSION`; `NIF_*` выбирают значимые поля; `NIM_MODIFY` только после успешного `NIM_ADD`; `NIM_DELETE` при выходе; фокус возвращают `NIM_SETFOCUS` после меню | [Shell_NotifyIconW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw), [NOTIFYICONDATAW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw), [The Taskbar](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar) |
| `TaskbarCreated` — сообщение, зарегистрированное shell (`RegisterWindowMessageW`, id 0xC000–0xFFFF, одинаковое в всех процессах); приём => иконку добавлять заново (в Windows 10 также при смене DPI) | [The Taskbar → Taskbar Creation Notification](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar), [RegisterWindowMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerwindowmessagew) |
| Hit-test панели: `ABM_GETTASKBARPOS` описывает **только системную** панель, поэтому для вторичных мониторов полоса считается как `rcMonitor − rcWork` | [SHAppBarMessage](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shappbarmessage), [GetMonitorInfoW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmonitorinfow), [MonitorFromPoint](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-monitorfrompoint), [WindowFromPoint](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-windowfrompoint), [GetAncestor](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getancestor) |
| Полноэкранный режим определяется через `GetForegroundWindow` + rect окна против `rcMonitor`, а не через `WS_EX_FULLSCREEN`: стиль `0x8000` в списке расширных стилей Learn **отсутствует** | [Extended Window Styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles), [GetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getforegroundwindow) |
| Уровень целостности для диагностики UIPI (`NIM_ADD err=5`): `TOKEN_MANDATORY_LABEL.Label` (не `Sid`), буфер = структура + место под SID, иначе `err=122`; `GetSidSubAuthorityCount` возвращает `PUCHAR` (количество), последнее значение получают как `GetSidSubAuthority(sid, *count - 1)` | [GetTokenInformation](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation), [TOKEN_MANDATORY_LABEL](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-token_mandatory_label), [TOKEN_INFORMATION_CLASS](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ne-winnt-token_information_class), [GetSidSubAuthorityCount](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-getsidsubauthoritycount), [GetSidSubAuthority](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-getsidsubauthority) |
| `IsUserAnAdmin` не используется: Learn называет его устаревшей обёрткой над `CheckTokenMembership` и рекомендует обращаться к токену напрямую | [IsUserAnAdmin](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-isuseranadmin) |
| Окно/десктоп-проверка: `UOI_NAME 2`, `UOI_TYPE 3`, `UOI_FLAGS 1`, `UOI_IO 6` («получает ли объект пользовательский ввод»); в MinGW `USEROBJECTFLAGS` = `fInherit/fReserved/dwFlags` (без `fRead`/`fWrite`) — десктоп `Default`: `UOI_IO = 1`, т.е. приём ввода не заблокирован | [GetUserObjectInformationW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getuserobjectinformationw), [USEROBJECTFLAGS](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-userobjectflags), [SetUserObjectInformationW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setuserobjectinformationw) |
| `GetLastError` читается сразу после неудачного вызова (часть функций обнуляют код при успехе) — ошибка `NIM_ADD err=5` фиксировалась до других вызовов | [GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror) |
| Watchdog построен на `WM_TIMER` (`lpTimerFunc = NULL`) с id `IDT_HOOKRETRY`; логируется только реальная неудача переустановки | [SetTimer](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-settimer) |

## Что взято из Volume2 (irzyxa/Volume2)

Приложение в репозитории Volume2 **не открыто**: там нет C/C++ исходников — только `Docs`, `History.txt`, языки, скины и картинки (проверено по дереву репозитория; единственный PowerShell-файл — `Languages/ExportLng.ps1`). Поэтому сверялись не с кодом, а с документацией и историей изменений:

- `Docs/OSD Skin - Media OSD and Buttons.md` подтверждает: OSD появляется при нажатии медиа-клавиш — это ровно тот механизм, который использует наша инъекция (`VK_VOLUME_UP/DOWN/MUTE`), поэтому собственный микшер не нужен.
- `History.txt` #389: колесо над **кнопкой** приложения меняет громкость этого приложения, над **пустым** местом панели — общую громкость. Мы сознательно ограничены пустой областью (per-app volume вне зафиксированной области).
- `History.txt` #373/#50: lag колеса на быстрых мышах/тачпадах лечится тем, что hook не делает работу сам — у нас hook только копирует событие и `PostMessage`-ит его.
- `History.txt` #424 и опция «auto restart hooks» (#512): хук переустанавливают после сна/гибернации — совпадает с нашим watchdog.
- Middle-click mute существует в Volume2 как отдельная функция — наша реализация повторяет поведение.
- `Docs/Skins - Colorization, previews and tray sizes.md`: размеры трей-иконок 16/20/24/32 px (по умолчанию 32) — учтено при подборе `icons\app.ico`.

## Ограничение среды проверки

В этой песочнице инъекция через `SendInput` не доходит до цепочки hook-ов (`events=0 injected=0` в self-injection пробах), а COM аудио недоступно (`CoCreateInstance → 0x80040154`), поэтому измерить фактическое изменение громкости здесь нельзя. additionally процессы, запущенные из harness, имеют Low IL (`S-1-16-4096`) против Medium IL Explorer (`S-1-16-8192`) — отсюда `NIM_ADD err=5`. Проверить работу нужно в обычном интерактивном сеансе: запустить `VolumeTray.exe`, прокрутить колесо над панелью задач и нажать среднюю кнопку над пустым местом, затем посмотреть строки `event=... passed=... sent=...` в логе при `Debug=1`.

