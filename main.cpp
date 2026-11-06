// VolumeTray — регулировка громкости текущего устройства вывода прокруткой
// мыши над панелью задач. Трей-меню: полноэкранный режим, средняя кнопка
// (mute), запуск при старте Windows, выход.
//
// Громкость меняется через SendInput(VK_VOLUME_*), поэтому Windows сама
// показывает свой системный индикатор и громкость применяется к текущему
// устройству вывода по умолчанию (Bluetooth / USB Audio / HDMI монитора).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnt.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

// Fallback для MinGW: эти константы есть в winuser.h/winnt.h MSVC, но не всегда в MinGW
#ifndef KEY_SET
#define KEY_SET          0x0002
#endif
#ifndef SE_GROUP_NAME_AND_COUNT_ASSISTING_SID
#define SE_GROUP_NAME_AND_COUNT_ASSISTING_SID 11
#endif
// WS_EX_FULLSCREEN (0x8000) НЕ описан в Microsoft Learn
// (https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles),
// поэтому он не используется. Полноэкранный режим определяется через
// GetForegroundWindow + GetWindowRect vs MonitorFromWindow/GetMonitorInfoW.
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

#define APP_NAME       L"VolumeTray"
#define TRAY_UID       1u
#define WM_TRAYICON    (WM_APP + 1)
#define WM_TRAYWORK    (WM_APP + 2)   // событие мыши из hook -> обработка в WndProc
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E         // горизонтальное колесо (MS_HWHEEL)
#endif
#define IDI_APPICON    101
#define IDT_HOOKRETRY  2u               // id таймера переустановки хука
#define HOOK_RETRY_MS  5000

#define IDM_FULL       1001   // работать в полноэкранном режиме
#define IDM_MUTE       1002   // средняя кнопка на таскбаре = mute
#define IDM_LOGIN      1003   // запуск при старте Windows
#define IDM_EXIT       1004

// Скорость: id 1100..1112 = шаги от -6 до +6 (1106 = 0).
// Значение шага равно номеру строки меню, поэтому submenu читается просто.
#define IDM_SPEED_BASE 1100

// Реверс колеса: простая галочка, меняет только направление громкости.
#define IDM_REVERSE    1113

// Гасить событие колеса/middle, когда его обработала громкость: приложение
// (например YouTube в полноэкранном) его не получает — иначе прокрутка над
// таскбаром ещё и скролит страницу / выкидывает ряд плиток.
#define IDM_SWALLOW    1114

static HINSTANCE  g_hInst;
static HWND       g_hWnd;
static HHOOK      g_hHook;
static NOTIFYICONDATAW g_nid = { sizeof(g_nid) };
static WCHAR      g_iniPath[MAX_PATH];
static WCHAR      g_exePath[MAX_PATH];
static UINT       g_uTaskbarCreated;
static HICON      g_hIcon = NULL;       // для DestroyIcon при выходе
static int        g_trayAdded = 0;      // первый NIM_ADD уже выполнен

static int g_fullscreen = 1;  // 1 = крутить громкость и под полноэкранным приложением
static int g_middleMute = 1;  // 1 = средняя кнопка на пустом месте таскбара = mute
static int g_debug = 0;       // 1 = писать каждое событие хука в %APPDATA%\VolumeTray.log
static int g_speed = 0;        // Скорость: число нажатий на одно движение колеса, от -6 до +6.
                               // 0 = один шаг (физически "0" не может означать "ничего").
                               // Знак инвертирует направление (как колесо мыши).
static int g_reverse = 0;      // Реверс колеса: галочка, меняет направление громкости
static int g_swallow = 1;      // Гасить колесо/middle, если его обработала громкость
static void LogLine(const char* line);
static int AddTrayIcon(void);
static void LoadIconAndTray(void);

static const WCHAR* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// --------------------------------------------------------------- настройки ---

static void LoadSettings()
{
    g_fullscreen = GetPrivateProfileIntW(L"Settings", L"Fullscreen", 1, g_iniPath);
    g_middleMute = GetPrivateProfileIntW(L"Settings", L"MiddleMute", 1, g_iniPath);
    g_debug = GetPrivateProfileIntW(L"Settings", L"Debug", 0, g_iniPath);
    g_speed = GetPrivateProfileIntW(L"Settings", L"Speed", 0, g_iniPath);
    if (g_speed < -6) g_speed = -6;
    if (g_speed > 6) g_speed = 6;
    g_reverse = GetPrivateProfileIntW(L"Settings", L"Reverse", 0, g_iniPath);
    g_swallow = GetPrivateProfileIntW(L"Settings", L"Swallow", 1, g_iniPath);
}

static void SaveSetting(const WCHAR* name, int val)
{
    WCHAR buf[8];
    wsprintfW(buf, L"%d", val);
    WritePrivateProfileStringW(L"Settings", name, buf, g_iniPath);
}

// ------------------------------------------------------------- elevation -----

static DWORD LastSubAuthority(PSID sid)
{
    if (!sid) return 0;
    PUCHAR cnt = GetSidSubAuthorityCount(sid);
    if (!cnt || *cnt == 0) return 0;
    PDWORD a = GetSidSubAuthority(sid, (DWORD)(*cnt) - 1);
    return a ? *a : 0;
}

// Реальный уровень целостности токена. UIPI: процесс с Low IL не может
// обращаться к объектам Medium/High IL — Explorer это Medium, поэтому
// Shell_NotifyIcon(NIM_ADD) возвращает FALSE / err=5 (ACCESS_DENIED).
static DWORD TokenIntegrity(void)
{
    // Буфер = TOKEN_MANDATORY_LABEL + место под SID; иначе err=122.
    static BYTE mem[512];
    DWORD len = 0;
    if (!GetTokenInformation(GetCurrentProcessToken(), TokenIntegrityLevel,
                             mem, sizeof(mem), &len))
        return 0xFFFFFFFFu;
    PTOKEN_MANDATORY_LABEL tml = (PTOKEN_MANDATORY_LABEL)mem;
    DWORD lab = LastSubAuthority(tml->Label.Sid);
    return lab ? lab : 0xFFFFFFFFu;
}

// Администратор ли процесс. IsUserAnAdmin — устаревшая обёртка над
// CheckTokenMembership (Learn рекомендует читать токен), и она возвращает TRUE
// для "filtered token" обычного админа без UAC-поднятия. Здесь нужны оба факта:
//   - membership в Alias\Builtin Administrators (S-1-5-32-544)
//   - полноценный токен (TokenElevationTypeFull), а не отфильтрованный
static int IsProcessElevated(void)
{
    HANDLE tok = GetCurrentProcessToken();
    static BYTE mem[1024];
    DWORD len = 0;

    int isAdmin = 0;
    if (GetTokenInformation(tok, TokenUser, mem, sizeof(mem), &len)) {
        PTOKEN_USER tu = (PTOKEN_USER)mem;
        if (LastSubAuthority(tu->User.Sid) == 544) isAdmin = 1;   // SID S-1-5-32-544
    }

    int fullToken = 0;
    if (GetTokenInformation(tok, TokenElevationType, mem, sizeof(mem), &len)) {
        TOKEN_ELEVATION_TYPE et = *(TOKEN_ELEVATION_TYPE*)mem;
        fullToken = (et == TokenElevationTypeFull);
    }

    // Отдельная проверка membership: токен может быть "restricted", и тогда
    // TokenUser не совпадает с реальными группами.
    if (!isAdmin) {
        static BYTE groups[4096];
        DWORD glen = 0;
        if (GetTokenInformation(tok, TokenGroups, groups, sizeof(groups), &glen)) {
            PTOKEN_GROUPS tg = (PTOKEN_GROUPS)groups;
            for (DWORD i = 0; i < tg->GroupCount; i++) {
                if (LastSubAuthority(tg->Groups[i].Sid) == 544 &&
                    (tg->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) == 0) {
                    isAdmin = 1;
                    break;
                }
            }
        }
    }

    return isAdmin && fullToken;
}

// Запуск при старте Windows. Реестр читается и пишется из HKCU — привилегий не
// требует. Но автозапуск даёт процесс с обычным (не повышенным) токеном, а такой
// процесс не может обратиться к Explorer (Medium IL) -> NIM_ADD err=5. Поэтому в
// Run пишется обёртка через rundll32 c вербом runas: при логоне она запрашивает
// повышение и только затем запускает exe. Rundll32 существует со времён Windows 7,
// путь всегда есть в system32.
static int GetLoginFromRegistry(void)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
        return 0;
    WCHAR val[1024];
    DWORD sz = sizeof(val), type = 0;
    LONG r = RegQueryValueExW(hk, APP_NAME, NULL, &type, (BYTE*)val, &sz);
    RegCloseKey(hk);
    if (r != ERROR_SUCCESS) return 0;
    // Считаем включённым любой вариант — прямой путь или rundll32-обёртка.
    return val[0] ? 1 : 0;
}

static void SetLoginInRegistry(int enabled)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET | KEY_READ, &hk) != ERROR_SUCCESS)
        return;
    if (enabled) {
        WCHAR val[1024];
        if (IsProcessElevated()) {
            // Уже повышен — прямой путь, без лишнего UAC-промпта при логоне.
            wsprintfW(val, L"\"%s\"", g_exePath);
        } else {
            // rundll32 с вербом runas: повышение при логоне.
            wsprintfW(val, L"rundll32 shell32,ShellExecByPath -runas \"%s\"", g_exePath);
        }
        RegSetValueExW(hk, APP_NAME, 0, REG_SZ, (const BYTE*)val,
                       (DWORD)((lstrlenW(val) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hk, APP_NAME);
    }
    RegCloseKey(hk);
}

// ----------------------------------------------------------------- утилиты ---

// Точка в полосе таскбара: между рабочей областью (rcWork) и краем монитора
// (rcMonitor) — это и есть полоса панели задач.
static BOOL PointInTaskbarBand(POINT p)
{
    HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONULL);
    if (!hm) return FALSE;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return FALSE;
    return PtInRect(&mi.rcMonitor, p) && !PtInRect(&mi.rcWork, p);
}

// Окно «пустого места» панели задач. На Win11 WindowFromPoint часто возвращает
// XAML-остров (island), а не сам Shell_TrayWnd. Считаем пустым местом, если
// класс окна — таск-бар или island-класс И точка лежит в полосе таскбара.
static BOOL IsEmptyTaskbarAt(POINT p)
{
    if (!PointInTaskbarBand(p)) return FALSE;
    HWND w = WindowFromPoint(p);
    if (!w) return FALSE;
    WCHAR cls[64];
    if (!GetClassNameW(w, cls, 64)) return FALSE;
    static const WCHAR* kTrayClasses[] = {
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"XamlExplorerHostIslandWindow", L"TaskbarFrame", L"InputSite.WindowClass",
    };
    for (size_t i = 0; i < sizeof(kTrayClasses) / sizeof(kTrayClasses[0]); i++)
        if (_wcsicmp(cls, kTrayClasses[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL PointOnTaskbar(POINT p)
{
    return IsEmptyTaskbarAt(p) || PointInTaskbarBand(p);
}

// Экранное «шел-окно», которое не надо считать полноэкранным приложением.
static BOOL IsShellForeground(HWND w)
{
    static const WCHAR* kExcluded[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"NotifyIconOverflowWindow", L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow", L"TopLevelWindowForOverflowXamlIsland",
    };
    WCHAR cls[96];
    if (!GetClassNameW(w, cls, 96)) return FALSE;
    for (size_t i = 0; i < sizeof(kExcluded) / sizeof(kExcluded[0]); i++)
        if (_wcsicmp(cls, kExcluded[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL IsFullscreenForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg || IsShellForeground(fg)) return FALSE;
    // Правило по документации: foreground window + её rect против rcMonitor.
    // WS_EX_FULLSCREEN (0x8000) в Learn не описан — не используем.
    if (IsZoomed(fg)) return FALSE; // с maximized-окном обычно видно панель задач
    HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return FALSE;
    RECT r;
    if (!GetWindowRect(fg, &r)) return FALSE;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
           r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// Системная клавиша громкости через SendInput — Windows сама рисует OSD
// и применяет к устройству, выбранному в системе по умолчанию.
// ВАЖНО: вызывается только из WndProc, НИКОГДА из hook-процедуры.
static int SendVolumeKey(WORD vk)
{
    INPUT in[2];
    ZeroMemory(in, sizeof(in));
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    UINT n = SendInput(2, in, sizeof(INPUT));
    if (n == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "SendInput failed err=%lu", GetLastError());
        LogLine(buf);
    }
    return (int)n;
}

// Копия события для передачи из hook в message loop.
typedef struct { int event; POINT pt; int delta; int handled; } EventRec;

// Решение «это событие обработает громкость» — нужно и в hook, чтобы знать,
// гасить ли событие. Разрешены только read-only проверки, которые не шлют
// сообщения другим процессам (WindowFromPoint/GetClassName не sendMessage):
// hook обязан вернуться за единицы мс, иначе его снимает LowLevelHooksTimeout.
static int VolumeWillHandle(POINT p, int event)
{
    if (event == WM_MBUTTONUP) return (g_middleMute && IsEmptyTaskbarAt(p)) ? 1 : 0;
    if (!PointOnTaskbar(p)) return 0;
    if (g_fullscreen) return 1;                       // крутим и под полноэкранным
    return IsFullscreenForeground() ? 0 : 1;
}

// Hook должен возвращаться за единицы миллисекунд: пока hook не вернулся,
// shell (XAML-таскбар) не может обработать (dispatch) событие — при
// превышении LowLevelHooksTimeout хук тихо снимается, а UI показывает
// «частичную занятость» курсора. Поэтому hook только копирует событие и
// PostMessage-ит его; вся логика — в WndProc (WM_TRAYWORK).
static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && g_hWnd && (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL || wp == WM_MBUTTONUP)) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lp;
        EventRec* rec = (EventRec*)malloc(sizeof(EventRec));
        if (rec) {
            rec->event = (int)wp;
            rec->pt = ms->pt;
            rec->delta = (int)(short)HIWORD(ms->mouseData); // signed delta
            rec->handled = VolumeWillHandle(ms->pt, (int)wp) ? 1 : 0;
            if (rec->handled && g_swallow) {
                // Событие гасим: ниже hook-а оно не пройдёт, приложение не
                // получит прокрутку (YouTube в полноэкранном перестанет
                // скроллить/показывать плитки, когда колесо над таскбаром).
                if (!PostMessageW(g_hWnd, WM_TRAYWORK, 0, (LPARAM)rec)) free(rec);
                return 1;
            }
            if (!PostMessageW(g_hWnd, WM_TRAYWORK, 0, (LPARAM)rec)) free(rec);
        }
    }
    return CallNextHookEx(g_hHook, code, wp, lp);
}

// Уровень целостности процесса (S-1-16-*). Нужен для диагностики NIM_ADD:
// UIPI запрещает lower-IL процессам слать сообщения/shell-вызовы в более
// высокий уровень (Explorer = Medium). Источник:
// https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changewindowmessagefilterex
static const char* IntegrityLevelName(void)
{
    static char buf[32];
    DWORD lab = TokenIntegrity();
    if (lab == 0xFFFFFFFFu) {
        snprintf(buf, sizeof(buf), "? err=%lu", (unsigned long)GetLastError());
        return buf;
    }
    // Уровни целостности (S-1-16-*): 0 Untrusted, 4096 Low, 8192 Medium,
    // 12288 High, 16384 System.
    const char* n = (lab == 0)     ? "Untrusted" : (lab == 4096)  ? "Low"   :
                    (lab == 8192)  ? "Medium"    : (lab == 12288) ? "High"  :
                    (lab == 16384) ? "System"    : "?";
    snprintf(buf, sizeof(buf), "%s(S-1-16-%lu)", n, (unsigned long)lab);
    return buf;
}

// Строка в лог %APPDATA%\VolumeTray.log (диагностика отказа хука)
static void LogLine(const char* line)
{
    if (!g_debug) return;   // лог пишется только при Debug=1
    WCHAR path[MAX_PATH];
    FILE* f = NULL;
    // Лог рядом с exe (надежно читается), затем fallback в %APPDATA%.
    lstrcpynW(path, g_exePath, MAX_PATH);
    WCHAR* sep = wcsrchr(path, L'\\');
    if (sep) lstrcpyW(sep + 1, L"VolumeTray.log");
    else lstrcatW(path, L"\\VolumeTray.log");
    f = _wfopen(path, L"a");   // "at" запрещён UIPI при High IL
    if (!f && GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH)) {
        lstrcatW(path, L"\\VolumeTray.log");
        f = _wfopen(path, L"a");   // UIPI при High IL
    }
    if (!f) return;
    fprintf(f, "%s\n", line);
    fclose(f);
}

static int g_shellIconSize = 32;   // размер, который реально использует shell

// Запуск самого себя с повышением. Verb "runas" — документированный способ
// запросить повышение через ShellExecuteW; UAC-промпт при этом появляется,
// и это ожидаемо: без повышения процесс остаётся с фильтрованным токеном
// (Low IL для процессов из песочницы / filtered token для админа), а UIPI
// запрещает ему обращаться к Explorer (Medium IL) -> NIM_ADD err=5.
static int SelfElevate(void)
{
    WCHAR dir[MAX_PATH];
    lstrcpynW(dir, g_exePath, MAX_PATH);
    WCHAR* sep = wcsrchr(dir, L'\\');
    if (sep) *sep = 0;

    HINSTANCE r = ShellExecuteW(NULL, L"runas", g_exePath, NULL, dir, SW_HIDE);
    return ((INT_PTR)r > 21) ? 1 : 0;
}

// Установка хука; при отказе — запись в лог и таймер на повторную попытку.
static void InstallHook(void)
{
    if (g_hHook) return;
    g_hHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_hInst, 0);
    if (!g_hHook) LogLine("SetWindowsHookEx(WH_MOUSE_LL) failed");
    else LogLine("hook installed");
    // Watchdog-таймер должен жить постоянно: Windows умеет тихо снимать
    // LL-хук по LowLevelHooksTimeout, и об этом нет никакого уведомления.
    if (g_hWnd) SetTimer(g_hWnd, IDT_HOOKRETRY, HOOK_RETRY_MS, NULL);
}

// ------------------------------------------------------------------ меню ----

static void ShowMenu()
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_FULL, L"Работать в полноэкранном режиме");
    AppendMenuW(m, MF_STRING, IDM_MUTE, L"Средняя кнопка на таскбаре \x2014 Mute");

    // Скорость: число нажатий на одно движение колеса. Подменю от -6 до +6.
    HMENU sm = CreatePopupMenu();
    for (int v = -6; v <= 6; v++) {
        WCHAR label[16];
        if (v > 0) wsprintfW(label, L"+%d", v);
        else if (v < 0) wsprintfW(label, L"%d", v);   // минус уже в строке
        else lstrcpyW(label, L"0");
        AppendMenuW(sm, MF_STRING, IDM_SPEED_BASE + v + 6, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sm, L"\x0421\x043a\x043e\x0440\x043e\x0441\x0442\x044c");

    // Реверс колеса: галочка. Меняет направление громкости, скорость не трогает.
    AppendMenuW(m, MF_STRING, IDM_REVERSE, L"\x0420\x0435\x0432\x0435\x0440\x0441 \x043a\x043e\x043b\x0435\x0441\x0430");

    // Гасить колесо: галочка. Если событие обработала громкость, оно не уходит
    // дальше по цепочке hook-ов (приложение под курсором его не увидит).
    AppendMenuW(m, MF_STRING, IDM_SWALLOW, L"\x0413\x0430\x0441\x0438\x0442\x044c \x043a\x043e\x043b\x0435\x0441\x043e (\x043d\x0435 \x043f\x0435\x0440\x0435\x0434\x0430\x0432\x0430\x0442\x044c \x0441\x0438\x0441\x0442\x0435\x043c\x0435)");

    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_LOGIN, L"Запускать при старте Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Выход");

    CheckMenuItem(m, IDM_FULL, g_fullscreen ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, IDM_MUTE, g_middleMute ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(sm, IDM_SPEED_BASE + g_speed + 6, MF_CHECKED);
    CheckMenuItem(m, IDM_REVERSE, g_reverse ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, IDM_SWALLOW, g_swallow ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(m, IDM_LOGIN, GetLoginFromRegistry() ? MF_CHECKED : MF_UNCHECKED);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hWnd); // чтобы меню закрывалось по клику вне
    int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                               p.x, p.y, g_hWnd, NULL);
    PostMessageW(g_hWnd, WM_NULL, 0, 0); // сброс «захвата» после меню
    // После закрытия меню возвращаем фокус области уведомлений:
    // https://learn.microsoft.com/en-us/windows/win32/shell/taskbar
    if (g_trayAdded) Shell_NotifyIconW(NIM_SETFOCUS, &g_nid);
    if (cmd == IDM_FULL)  { g_fullscreen ^= 1;  SaveSetting(L"Fullscreen", g_fullscreen); }
    else if (cmd == IDM_MUTE) { g_middleMute ^= 1; SaveSetting(L"MiddleMute", g_middleMute); }
    else if (cmd >= IDM_SPEED_BASE && cmd <= IDM_SPEED_BASE + 12) {
        g_speed = cmd - IDM_SPEED_BASE - 6;
        SaveSetting(L"Speed", g_speed);
    }
    else if (cmd == IDM_REVERSE) { g_reverse ^= 1; SaveSetting(L"Reverse", g_reverse); }
    else if (cmd == IDM_SWALLOW) { g_swallow ^= 1; SaveSetting(L"Swallow", g_swallow); }
    else if (cmd == IDM_LOGIN) {
        SetLoginInRegistry(GetLoginFromRegistry() ? 0 : 1);
    }
    else if (cmd == IDM_EXIT) { PostMessageW(g_hWnd, WM_CLOSE, 0, 0); }
    DestroyMenu(sm);
    DestroyMenu(m);
}

// ------------------------------------------------------------- окно/трей -----

static int AddTrayIcon(void)
{
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hWnd = g_hWnd;
    g_nid.uID = TRAY_UID;
    lstrcpynW(g_nid.szTip, L"VolumeTray", ARRAYSIZE(g_nid.szTip));

    if (!g_trayAdded) {
        // Первая регистрация — NIM_ADD. ПОРЯДОК ВАЖЕН: при NIM_ADD поле
        // version должно быть 0; версию задаём отдельным NIM_SETVERSION после.
        // (version != 0 на ADD -> Shell_NotifyIcon возвращает FALSE.)
        if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) {
            DWORD err = GetLastError();   // читаем ДО других вызовов
            char buf[160];
            snprintf(buf, sizeof(buf), "NIM_ADD failed err=%lu cbSize=%u IL=%s",
                     (unsigned long)err, (unsigned)g_nid.cbSize, IntegrityLevelName());
            LogLine(buf);
            // UIPI: lower-IL процесс не может обратиться к Explorer (Medium IL).
            // Ошибка 5 = ACCESS_DENIED. Это единственная причина, по которой
            // приложение "работает только от админа" — сам код привилегий не требует.
            if (err == ERROR_ACCESS_DENIED) {
                MessageBoxW(NULL,
                    L"Иконку в трее добавить не удалось: процесс работает с низким "
                    L"уровнем целостности (UIPI запрещает обращение к Explorer).\n\n"
                    L"Запустите VolumeTray от имени администратора (или включите "
                    L"«Запускать при старте Windows» — там используется обёртка с "
                    L"запросом повышения).",
                    L"VolumeTray", MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        g_trayAdded = 1;
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &g_nid))
            LogLine("NIM_SETVERSION failed (fallback: v0)");
    } else {
        // Повторная регистрация (TaskbarCreated) — NIM_MODIFY.
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
    return 1;
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON: {
        // NOTIFYICON_VERSION_4: uID в LOWORD(lp), событие в HIWORD(lp).
        // Для старой версии (без SETVERSION) событие было в LOWORD.
        // Проверяем оба формата. Скобки обязательны: инициализация переменной
        // внутри case без блока ломает switch в C++.
        WORD ev = (LOWORD(lp) == TRAY_UID) ? HIWORD(lp) : LOWORD(lp);
        if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU || ev == WM_LBUTTONUP)
            ShowMenu();
        return 0;
    }
    case WM_TRAYWORK: {
        // Обработка события мыши, переданного из hook (PostMessage).
        // Здесь можно делать «тяжёлые» проверки — это message loop, не hook.
        EventRec* rec = (EventRec*)lp;
        if (!rec) return 0;
        int passed = rec->handled, sent = 0;
        // MSLLHOOKSTRUCT.pt — экранные координаты в той же системе, что и
        // GetMonitorInfoW/WindowFromPoint для shell. Делить на dpif/96 нельзя:
        // при 150% масштабании точка уходит за границу монитора (y=2120 > 1440),
        // hit-test не проходит и громкость не меняется.
        POINT p = rec->pt;
        if (passed) {
            if (rec->event == WM_MBUTTONUP) {
                sent = SendVolumeKey(VK_VOLUME_MUTE);
            } else {
                // Скорость = число нажатий на одно движение колеса.
                // Реверс (галочка) меняет направление громкости. Знак скорости тоже
                // инвертирует, поэтому два знака складываются через XOR.
                int n = g_speed < 0 ? -g_speed : g_speed;
                if (n == 0) n = 1;   // «0» — это один шаг, а не «ничего не отправлять»
                int inverted = ((g_speed < 0) ^ (g_reverse != 0)) ? 1 : 0;
                WORD vk = (rec->delta > 0) ? VK_VOLUME_UP : VK_VOLUME_DOWN;
                if (inverted) vk = (rec->delta > 0) ? VK_VOLUME_DOWN : VK_VOLUME_UP;
                for (int i = 0; i < n; i++) {
                    int r = SendVolumeKey(vk);
                    if (r == 0) break;   // SendInput отказал — не крутим дальше
                    sent += r;
                }
            }
        }
        if (g_debug) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "event=%X pt=%ld,%ld passed=%d sent=%u swallow=%d",
                     rec->event, p.x, p.y, passed, sent,
                     (passed && g_swallow) ? 1 : 0);
            LogLine(buf);
        }
        free(rec);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_FULL:  g_fullscreen ^= 1; SaveSetting(L"Fullscreen", g_fullscreen); break;
        case IDM_MUTE:  g_middleMute ^= 1; SaveSetting(L"MiddleMute", g_middleMute); break;
        case IDM_REVERSE: g_reverse ^= 1; SaveSetting(L"Reverse", g_reverse); break;
        case IDM_SWALLOW: g_swallow ^= 1; SaveSetting(L"Swallow", g_swallow); break;
        case IDM_LOGIN: SetLoginInRegistry(GetLoginFromRegistry() ? 0 : 1); break;
        case IDM_EXIT:  PostMessageW(hWnd, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_TIMER:
        // Watchdog: Windows может ТИХО снять LL-хук по LowLevelHooksTimeout
        // (g_hHook при этом остаётся non-NULL, узнать о снятии нельзя) —
        // поэтому периодически переустанавливаем принудительно.
        if (g_hHook) {
            UnhookWindowsHookEx(g_hHook);
            g_hHook = NULL;
            InstallHook();
            // Логируем только фактическую неудачу переустановки: hook мог быть
            // тихо снят по LowLevelHooksTimeout, и узнать об этом нельзя.
            if (!g_hHook && g_debug) LogLine("hook watchdog: reinstall failed");
        } else {
            InstallHook();
        }
        return 0;
    case WM_CLOSE:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        KillTimer(hWnd, IDT_HOOKRETRY);
        if (g_hIcon) { DestroyIcon(g_hIcon); g_hIcon = NULL; }
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_uTaskbarCreated) { AddTrayIcon(); return 0; }
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// Размер иконки, который выбирает shell для области уведомлений: зависит от DPI.
// При 100% (96 dpi) классически это 16 px, Windows 11 рисует 32 px (физические).
static int GetShellIconSize(void)
{
    int dpi = 96;
    HDC dc = GetDC(NULL);
    if (dc) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    int sz = 16 * dpi / 96;
    // Windows 11 shell использует физические 32 px при 100%; держаем минимум 32.
    if (sz < 32) sz = 32;
    if (sz > 256) sz = 256;
    return sz;
}

static void LoadIconAndTray(void)
{
    // Размер, который реально нужен трю: Windows сама выбирает запись из .ico по
    // DPI. Если бы в .ico была только одна большая запись, shell масштабировал бы
    // её вниз и иконка выглядела «странно» — размытой/срезанной. Поэтому app.ico
    // содержит все размеры от 16 до 256, а LoadIconW по group-ресурсу подбирает
    // ближайшую запись к нужному размеру.
    int best = g_shellIconSize;

    HRSRC hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), RT_GROUP_ICON);
    if (hRes) {
        HGLOBAL hg = LoadResource(g_hInst, hRes);
        BYTE* grp = (BYTE*)LockResource(hg);
        if (grp) {
            // ICONGROUPHEADER: DWORD reserved, WORD count, WORD type.
            // count — на offset 4 (проверено на app.ico), а не 2.
            WORD count = *(WORD*)(grp + 4);          // ICONGROUPHEADER.count
            int bestArea = 0x7fffffff;
            char bestDesc[16] = "";
            for (WORD i = 0; i < count; i++) {
                BYTE* e = grp + 6 + 14 * i;          // IcoDirEntry в группе: 14 байта
                int w = e[0] ? e[0] : 256;
                int h = e[1] ? e[1] : 256;
                int area = (w - best) * (w - best) + (h - best) * (h - best);
                if (area < bestArea) {
                    bestArea = area;
                    snprintf(bestDesc, sizeof(bestDesc), "%dx%d", w, h);
                }
            }
            // Логируем только итоговый выбор: иначе 9 строк «icon pick»,
            // и последняя — не тот размер, который LoadIconW реально возьмёт.
            if (bestDesc[0]) {
                char buf[64];
                snprintf(buf, sizeof(buf), "icon pick %s for shell %d", bestDesc, best);
                LogLine(buf);
            }
        }
    }

    HICON hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!hIcon) hIcon = LoadIcon(NULL, IDI_APPLICATION); // fallback
    if (hIcon) {
        g_hIcon = hIcon;
        g_nid.hIcon = hIcon;
        AddTrayIcon();
    } else {
        LogLine("LoadIconW(IDI_APPICON) failed");
    }
}

// ------------------------------------------------------------------ main -----

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    g_hInst = hInst;
    // DPI awareness задаётся manifest-ом (PerMonitorV2). SetProcessDPIAware()
    // здесь не нужен: он делает процесс System-DPI-aware, и координаты hook
    // (физические пиксели) снова расходятся с виртуализированными.
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();  // fallback для старых версий Windows
    }

    // Local\ (per-session) не требует привилегий, в отличие от Global\-namespace.
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\" APP_NAME L".singleinstance");
    if (!hMutex) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        // Второй экземпляр: не молча выходим, а сообщаем, что уже запущено.
        MessageBoxW(NULL, L"VolumeTray уже запущен (иконка в трее).",
                    L"VolumeTray", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    lstrcpyW(g_iniPath, g_exePath);
    // точка берётся только после последнего сепаратора — иначе путь с '.'
    // в имени папки портится
    WCHAR* sep = wcsrchr(g_iniPath, L'\\');
    WCHAR* dot = sep ? wcsrchr(sep, L'.') : NULL;
    if (dot) *dot = 0;
    lstrcatW(g_iniPath, L".ini");

    // Fallback: пробуем записать маркер в папку exe; если нельзя
    // (например Program Files) — ini в %APPDATA%\VolumeTray.ini
    HANDLE marker = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    if (marker == INVALID_HANDLE_VALUE) {
        WCHAR appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) {
            lstrcatW(appdata, L"\\VolumeTray.ini");
            lstrcpyW(g_iniPath, appdata);
        }
    } else {
        CloseHandle(marker);
    }

    LoadSettings();

    {
        char buf[128];
        snprintf(buf, sizeof(buf), "started IL=%s elevated=%d",
                 IntegrityLevelName(), IsProcessElevated());
        LogLine(buf);
    }

    // Если процесс не повышен — просим повышение и выходим: в повышенном
    // процессе UIPI не мешает Shell_NotifyIcon. Без повышения приложение
    // физически не может добавить иконку (NIM_ADD err=5), поэтому попытка
    // "работать как обычно" бессмысленна.
    if (!IsProcessElevated()) {
        if (SelfElevate()) {
            LogLine("self-elevate requested, exiting");
            CloseHandle(hMutex);
            return 0;
        }
        LogLine("self-elevate failed");
        // Продолжаем без повышения: часть функций (хук, колесо) работает,
        // но иконка в трее не появится. Пользователь видит причину в сообщении.
    }

    g_shellIconSize = GetShellIconSize();

    g_uTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"VolumeTrayHiddenWnd";
    if (!RegisterClassW(&wc)) { LogLine("RegisterClassW failed"); return 0; }

    // Скрытое top-level окно: на message-only (HWND_MESSAGE) не приходит
    // TaskbarCreated (HWND_BROADCAST — только top-level) и не работает
    // SetForegroundWindow. Окно не показываем: ShowWindow не вызываем,
    // размеры 0.
    g_hWnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME, WS_OVERLAPPED,
                             0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hWnd) { LogLine("CreateWindowExW failed"); return 0; }

    LoadIconAndTray();

    InstallHook();

    // MSG loop (нужен и для WH_MOUSE_LL, и для меню)
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hHook) UnhookWindowsHookEx(g_hHook);
    if (g_hIcon) { DestroyIcon(g_hIcon); g_hIcon = NULL; }
    CloseHandle(hMutex);
    return 0;
}
