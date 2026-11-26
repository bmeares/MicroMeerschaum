// SPDX-License-Identifier: Apache-2.0
//! Interactive and piped MicroMeerschaum device shell (`mrsm micro shell -i micro:<label>`).
use crate::device::{error, exchange};
use meerschaum_plugin::prelude::*;
use reedline::{
    Color as ReedlineColor, ColumnarMenu, Completer, CompletionResult, DefaultPrompt, Emacs,
    FileBackedHistory, Hinter, History, KeyCode, KeyModifiers, MenuBuilder, Prompt, PromptEditMode,
    PromptHistorySearch, Reedline, ReedlineEvent, ReedlineMenu, Signal, Span, Suggestion,
    default_emacs_keybindings,
};
use std::{
    borrow::Cow,
    env, fs,
    io::{self, IsTerminal, Write},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

const VERSION: &str = env!("CARGO_PKG_VERSION");
const DEFAULT_MAX_HISTORY: usize = 1000;
const TOOLBAR_BASE_SGR: &str = "0;38;5;16;7";

#[derive(Debug, Clone)]
struct ShellTheme {
    ansi: bool,
    unicode: bool,
    prompt_template: String,
    prompt_style: String,
    username_style: String,
    instance_style: String,
    compose_style: String,
    executor_style: String,
    intro_style: String,
    close_message: String,
    close_style: String,
    success_icon: String,
    success_style: String,
    failure_icon: String,
    failure_style: String,
    connected_icon: String,
    connected_style: String,
    disconnected_icon: String,
    disconnected_style: String,
    toolbar_enabled: bool,
    max_history: usize,
    executor_keys: String,
}

impl ShellTheme {
    fn from_host(interactive: bool) -> Result<Self> {
        let unicode = host::config(&["formatting", "unicode"])? != false;
        let ansi = interactive
            && host::config(&["formatting", "ansi"])? != false
            && !no_color_env(env::var_os("NO_COLOR").as_deref());
        let charset = if unicode { "unicode" } else { "ascii" };

        let opt_str = |keys: &[&str]| -> Option<String> {
            host::config(keys)
                .ok()
                .and_then(|v| v.as_str().map(str::to_owned))
        };
        let non_empty_str = |keys: &[&str], fallback: &str| -> String {
            opt_str(keys)
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| fallback.to_owned())
        };
        let icon_str = |keys: &[&str], fallback: &str| -> String {
            opt_str(keys).unwrap_or_else(|| fallback.to_owned())
        };

        let toolbar_enabled = host::config(&["shell", "bottom_toolbar", "enabled"])
            .ok()
            .and_then(|v| v.as_bool())
            .unwrap_or(true);
        let max_history = host::config(&["shell", "max_history"])
            .ok()
            .and_then(|v| {
                v.as_u64()
                    .or_else(|| v.as_i64().and_then(|n| u64::try_from(n).ok()))
            })
            .map(|n| (n as usize).max(1))
            .unwrap_or(DEFAULT_MAX_HISTORY);

        Ok(Self {
            ansi,
            unicode,
            prompt_template: non_empty_str(
                &["shell", charset, "prompt"],
                if unicode {
                    "\n [ {username}@{instance}{compose} ] ➤ "
                } else {
                    "\n [ {username}@{instance}{compose} ] > "
                },
            ),
            prompt_style: non_empty_str(&["shell", "ansi", "prompt", "rich", "style"], "green"),
            username_style: non_empty_str(&["shell", "ansi", "username", "rich", "style"], "white"),
            instance_style: non_empty_str(&["shell", "ansi", "instance", "rich", "style"], "cyan"),
            compose_style: non_empty_str(
                &["shell", "ansi", "compose", "rich", "style"],
                "bright_magenta",
            ),
            executor_style: non_empty_str(
                &["shell", "ansi", "executor", "rich", "style"],
                "yellow",
            ),
            intro_style: non_empty_str(
                &["shell", "ansi", "intro", "rich", "style"],
                "bold bright_blue",
            ),
            close_message: non_empty_str(
                &["shell", charset, "close_message"],
                "Thank you for using MicroMeerschaum!",
            ),
            close_style: non_empty_str(
                &["shell", "ansi", "close_message", "rich", "style"],
                "bright_blue",
            ),
            success_icon: icon_str(
                &["formatting", "success", charset, "icon"],
                if unicode { "🎉" } else { "+" },
            ),
            success_style: non_empty_str(
                &["formatting", "success", "ansi", "rich", "style"],
                "bold bright_green",
            ),
            failure_icon: icon_str(
                &["formatting", "failure", charset, "icon"],
                if unicode { "💢" } else { "-" },
            ),
            failure_style: non_empty_str(
                &["formatting", "failure", "ansi", "rich", "style"],
                "bold red",
            ),
            connected_icon: icon_str(
                &["formatting", "connected", charset, "icon"],
                if unicode { "🟢" } else { "" },
            ),
            connected_style: non_empty_str(
                &["formatting", "connected", "ansi", "rich", "style"],
                "green",
            ),
            disconnected_icon: icon_str(
                &["formatting", "disconnected", charset, "icon"],
                if unicode { "🔴" } else { "" },
            ),
            disconnected_style: non_empty_str(
                &["formatting", "disconnected", "ansi", "rich", "style"],
                "red",
            ),
            toolbar_enabled,
            max_history,
            executor_keys: non_empty_str(&["meerschaum", "executor"], "local"),
        })
    }
}

fn no_color_env(val: Option<&std::ffi::OsStr>) -> bool {
    val.is_some_and(|v| !v.is_empty())
}

fn env_non_empty(key: &str) -> Option<String> {
    env::var(key).ok().filter(|v| !v.is_empty())
}

fn compose_project_name(raw: Option<&str>) -> Option<String> {
    let parsed: Value = serde_json::from_str(raw?).ok()?;
    if let Some(name) = parsed
        .get("project_name")
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
    {
        return Some(name.to_owned());
    }
    let file = parsed.get("__file__").and_then(Value::as_str)?;
    PathBuf::from(file)
        .parent()
        .and_then(|p| p.file_stem())
        .map(|s| s.to_string_lossy().into_owned())
        .filter(|s| !s.is_empty())
}

fn transport_label(config: &Value) -> String {
    match config["transport"].as_str().unwrap_or("serial") {
        "usb" | "serial" | "" => "serial".to_owned(),
        other => other.to_owned(),
    }
}

fn history_path_from_env(
    xdg_state_home: Option<&str>,
    local_app_data: Option<&str>,
    home: Option<&str>,
    userprofile: Option<&str>,
) -> Option<PathBuf> {
    let base = xdg_state_home
        .filter(|s| !s.is_empty())
        .or_else(|| local_app_data.filter(|s| !s.is_empty()))
        .map(PathBuf::from)
        .or_else(|| {
            home.filter(|s| !s.is_empty())
                .or_else(|| userprofile.filter(|s| !s.is_empty()))
                .map(|h| PathBuf::from(h).join(".local").join("state"))
        })?;
    Some(base.join("micromeerschaum").join("shell_history"))
}

fn history_path() -> Option<PathBuf> {
    history_path_from_env(
        env_non_empty("XDG_STATE_HOME").as_deref(),
        env_non_empty("LOCALAPPDATA").as_deref(),
        env_non_empty("HOME").as_deref(),
        env_non_empty("USERPROFILE").as_deref(),
    )
}

fn build_history(max_history: usize, path: Option<PathBuf>) -> FileBackedHistory {
    if let Some(path) = path {
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        if let Ok(history) = FileBackedHistory::with_file(max_history, path) {
            return history;
        }
    }
    FileBackedHistory::new(max_history).unwrap_or_default()
}

fn terminal_size() -> (usize, usize) {
    if let Some((cols, rows)) = ioctl_terminal_size()
        && cols > 0
        && rows > 0
    {
        return (cols, rows);
    }
    let cols = env_non_empty("COLUMNS")
        .and_then(|v| v.parse::<usize>().ok())
        .filter(|&n| n > 0)
        .unwrap_or(80);
    let rows = env_non_empty("LINES")
        .and_then(|v| v.parse::<usize>().ok())
        .filter(|&n| n > 0)
        .unwrap_or(24);
    (cols, rows)
}

#[cfg(any(
    target_os = "linux",
    target_os = "android",
    target_os = "macos",
    target_os = "freebsd",
    target_os = "openbsd",
    target_os = "netbsd"
))]
fn ioctl_terminal_size() -> Option<(usize, usize)> {
    #[repr(C)]
    struct Winsize {
        ws_row: u16,
        ws_col: u16,
        ws_xpixel: u16,
        ws_ypixel: u16,
    }
    #[cfg(any(target_os = "linux", target_os = "android"))]
    const TIOCGWINSZ: std::ffi::c_ulong = 0x5413;
    #[cfg(any(
        target_os = "macos",
        target_os = "freebsd",
        target_os = "openbsd",
        target_os = "netbsd"
    ))]
    const TIOCGWINSZ: std::ffi::c_ulong = 0x4008_7468;

    unsafe extern "C" {
        fn ioctl(fd: i32, request: std::ffi::c_ulong, ...) -> i32;
    }

    let mut ws = Winsize {
        ws_row: 0,
        ws_col: 0,
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    let rc = unsafe { ioctl(1, TIOCGWINSZ, &mut ws as *mut Winsize) };
    if rc == 0 && ws.ws_col > 0 && ws.ws_row > 0 {
        Some((ws.ws_col as usize, ws.ws_row as usize))
    } else {
        None
    }
}

#[cfg(not(any(
    target_os = "linux",
    target_os = "android",
    target_os = "macos",
    target_os = "freebsd",
    target_os = "openbsd",
    target_os = "netbsd"
)))]
fn ioctl_terminal_size() -> Option<(usize, usize)> {
    None
}

// --- ANSI SGR parser & renderer (zero extra dependencies) ---

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum AnsiColor {
    Default,
    Standard(u8),
    EightBit(u8),
    Rgb(u8, u8, u8),
}

impl AnsiColor {
    fn parse(token: &str) -> Option<Self> {
        let normalized = token.trim().to_ascii_lowercase().replace('-', "_");
        if normalized == "default" {
            return Some(Self::Default);
        }
        if let Some(hex) = normalized.strip_prefix('#') {
            if hex.len() == 6 && hex.chars().all(|c| c.is_ascii_hexdigit()) {
                let r = u8::from_str_radix(&hex[0..2], 16).ok()?;
                let g = u8::from_str_radix(&hex[2..4], 16).ok()?;
                let b = u8::from_str_radix(&hex[4..6], 16).ok()?;
                return Some(Self::Rgb(r, g, b));
            }
            return None;
        }
        if let Some(inner) = normalized
            .strip_prefix("color(")
            .and_then(|s| s.strip_suffix(')'))
        {
            let n: u8 = inner.trim().parse().ok()?;
            return Some(if n < 16 {
                Self::Standard(n)
            } else {
                Self::EightBit(n)
            });
        }
        if let Some(inner) = normalized
            .strip_prefix("rgb(")
            .and_then(|s| s.strip_suffix(')'))
        {
            let parts: Vec<&str> = inner.split(',').collect();
            if parts.len() == 3 {
                let r: u8 = parts[0].trim().parse().ok()?;
                let g: u8 = parts[1].trim().parse().ok()?;
                let b: u8 = parts[2].trim().parse().ok()?;
                return Some(Self::Rgb(r, g, b));
            }
            return None;
        }
        let code = match normalized.as_str() {
            "black" => return Some(Self::Standard(0)),
            "red" => return Some(Self::Standard(1)),
            "green" => return Some(Self::Standard(2)),
            "yellow" => return Some(Self::Standard(3)),
            "blue" => return Some(Self::Standard(4)),
            "magenta" => return Some(Self::Standard(5)),
            "cyan" => return Some(Self::Standard(6)),
            "white" => return Some(Self::Standard(7)),
            "bright_black" | "grey" | "gray" => return Some(Self::Standard(8)),
            "bright_red" => return Some(Self::Standard(9)),
            "bright_green" => return Some(Self::Standard(10)),
            "bright_yellow" => return Some(Self::Standard(11)),
            "bright_blue" => return Some(Self::Standard(12)),
            "bright_magenta" => return Some(Self::Standard(13)),
            "bright_cyan" => return Some(Self::Standard(14)),
            "bright_white" => return Some(Self::Standard(15)),
            "pale_green3" => 114,
            "indian_red" => 167,
            "purple" => 129,
            _ => return None,
        };
        Some(Self::EightBit(code))
    }

    fn push_sgr(self, foreground: bool, out: &mut Vec<String>) {
        match self {
            Self::Default => out.push(if foreground { "39" } else { "49" }.to_owned()),
            Self::Standard(n) => {
                let base = match (foreground, n < 8) {
                    (true, true) => 30,
                    (false, true) => 40,
                    (true, false) => 82,
                    (false, false) => 92,
                };
                out.push((base + n as u32).to_string());
            }
            Self::EightBit(n) => {
                out.push(if foreground { "38" } else { "48" }.to_owned());
                out.push("5".to_owned());
                out.push(n.to_string());
            }
            Self::Rgb(r, g, b) => {
                out.push(if foreground { "38" } else { "48" }.to_owned());
                out.push("2".to_owned());
                out.push(r.to_string());
                out.push(g.to_string());
                out.push(b.to_string());
            }
        }
    }
}

fn style_to_sgr(style: &str) -> String {
    let trimmed = style.trim();
    if trimmed.is_empty() || trimmed.eq_ignore_ascii_case("none") {
        return String::new();
    }
    let words: Vec<&str> = trimmed.split_whitespace().collect();
    let mut attrs: Vec<&'static str> = Vec::new();
    let mut fg: Option<AnsiColor> = None;
    let mut bg: Option<AnsiColor> = None;
    let mut i = 0;

    while i < words.len() {
        let w = words[i].to_ascii_lowercase();
        match w.as_str() {
            "on" => {
                i += 1;
                if i < words.len() {
                    if let Some((color, consumed)) = parse_color_at(&words, i) {
                        bg = Some(color);
                        i += consumed;
                        continue;
                    }
                    i += 1;
                }
            }
            "not" => {
                i += 2;
            }
            "bold" | "b" => {
                push_unique(&mut attrs, "1");
                i += 1;
            }
            "dim" | "d" => {
                push_unique(&mut attrs, "2");
                i += 1;
            }
            "italic" | "i" => {
                push_unique(&mut attrs, "3");
                i += 1;
            }
            "underline" | "u" => {
                push_unique(&mut attrs, "4");
                i += 1;
            }
            "blink" => {
                push_unique(&mut attrs, "5");
                i += 1;
            }
            "reverse" | "r" => {
                push_unique(&mut attrs, "7");
                i += 1;
            }
            "strike" | "s" => {
                push_unique(&mut attrs, "9");
                i += 1;
            }
            _ => {
                if let Some((color, consumed)) = parse_color_at(&words, i) {
                    fg = Some(color);
                    i += consumed;
                } else {
                    i += 1;
                }
            }
        }
    }

    let mut codes: Vec<String> = attrs.into_iter().map(str::to_owned).collect();
    if let Some(c) = fg {
        c.push_sgr(true, &mut codes);
    }
    if let Some(c) = bg {
        c.push_sgr(false, &mut codes);
    }
    codes.join(";")
}

fn parse_color_at(words: &[&str], index: usize) -> Option<(AnsiColor, usize)> {
    if index + 1 < words.len() {
        let joined = format!("{}_{}", words[index], words[index + 1]);
        if let Some(c) = AnsiColor::parse(&joined) {
            return Some((c, 2));
        }
    }
    AnsiColor::parse(words[index]).map(|c| (c, 1))
}

fn push_unique(vec: &mut Vec<&'static str>, val: &'static str) {
    if !vec.contains(&val) {
        vec.push(val);
    }
}

fn apply_style(text: &str, style: &str, ansi: bool) -> String {
    if !ansi || text.is_empty() {
        return text.to_owned();
    }
    let sgr = style_to_sgr(style);
    if sgr.is_empty() {
        return text.to_owned();
    }
    format!("\x1b[{sgr}m{text}\x1b[0m")
}

fn apply_style_multiline(text: &str, style: &str, ansi: bool) -> String {
    if !ansi || text.is_empty() {
        return text.to_owned();
    }
    let sgr = style_to_sgr(style);
    if sgr.is_empty() {
        return text.to_owned();
    }
    text.split('\n')
        .map(|line| {
            if line.is_empty() {
                String::new()
            } else {
                format!("\x1b[{sgr}m{line}\x1b[0m")
            }
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn char_display_width(c: char) -> usize {
    let cp = c as u32;
    if cp < 0x20 || cp == 0x7F {
        return 0;
    }
    if matches!(
        cp,
        0x0300..=0x036F | 0x200B..=0x200F | 0xFE00..=0xFE0F | 0xE0100..=0xE01EF
    ) {
        return 0;
    }
    if matches!(
        cp,
        0x1100..=0x115F
            | 0x231A..=0x231B
            | 0x23E9..=0x23EC
            | 0x23F0
            | 0x23F3
            | 0x25FD..=0x25FE
            | 0x2614..=0x2615
            | 0x2648..=0x2653
            | 0x267F
            | 0x2693
            | 0x26A1
            | 0x26AA..=0x26AB
            | 0x26BD..=0x26BE
            | 0x26C4..=0x26C5
            | 0x26CE
            | 0x26D4
            | 0x26EA
            | 0x26F2..=0x26F3
            | 0x26F5
            | 0x26FA
            | 0x26FD
            | 0x2705
            | 0x270A..=0x270B
            | 0x2728
            | 0x274C
            | 0x274E
            | 0x2753..=0x2755
            | 0x2757
            | 0x2795..=0x2797
            | 0x27B0
            | 0x27BF
            | 0x2E80..=0xA4CF
            | 0xAC00..=0xD7A3
            | 0xF900..=0xFAFF
            | 0xFE10..=0xFE19
            | 0xFE30..=0xFE6F
            | 0xFF00..=0xFF60
            | 0xFFE0..=0xFFE6
            | 0x1F000..=0x1FAFF
            | 0x20000..=0x3FFFD
    ) {
        return 2;
    }
    1
}

fn display_width(s: &str) -> usize {
    s.chars().map(char_display_width).sum()
}

fn truncate_text_for_display(text: &str, max_length: usize, suffix: &str) -> String {
    let len = text.chars().count();
    if len <= max_length {
        return text.to_owned();
    }
    if max_length == 0 {
        return String::new();
    }
    let suffix_len = suffix.chars().count();
    if max_length <= suffix_len {
        return suffix.chars().take(max_length).collect();
    }
    let keep = max_length - suffix_len;
    format!("{}{suffix}", text.chars().take(keep).collect::<String>())
}

// --- Banner, close message, prompt, results, toolbar ---

fn render_banner(theme: &ShellTheme, version: &str) -> String {
    let art = if theme.unicode {
        concat!(
            " █▄ ▄█ ▀ ▄▀▀ █▀▄ ▄▀▄  █▄ ▄█ █▀▄ ▄▀▀ █▄ ▄█\n",
            " █ ▀ █ █ ▀▄▄ █▀▄ ▀▄▀  █ ▀ █ █▀▄ ▄██ █ ▀ █"
        )
    } else {
        concat!(
            " |\\/| .  __  __  __   |\\/|  __  __  |\\/|\n",
            " |  | | /   |__) /  \\ |  | |__) __\\ |  |"
        )
    };
    let width = art.lines().map(display_width).max().unwrap_or(0);
    let label = " MicroMeerschaum";
    let tag = format!("v{version}");
    let pad = width
        .saturating_sub(display_width(label))
        .saturating_sub(display_width(&tag))
        .max(1);
    let banner = format!("\n{art}\n{label}{}{tag}", " ".repeat(pad));
    apply_style_multiline(&banner, &theme.intro_style, theme.ansi)
}

fn render_close_message(theme: &ShellTheme) -> String {
    format!(
        "\n{}\n",
        apply_style(&theme.close_message, &theme.close_style, theme.ansi)
    )
}

struct PromptContext<'a> {
    username: &'a str,
    instance: &'a str,
    compose: Option<&'a str>,
    executor_keys: &'a str,
}

fn render_prompt(theme: &ShellTheme, ctx: &PromptContext<'_>, term_width: usize) -> String {
    let suffix = if theme.unicode { "…" } else { "..." };
    let max_length = term_width / 4;

    let style_field = |value: &str, style: &str| {
        let truncated = truncate_text_for_display(value, max_length, suffix);
        apply_style(&truncated, style, theme.ansi)
    };

    let username = style_field(ctx.username, &theme.username_style);
    let instance = style_field(ctx.instance, &theme.instance_style);
    let executor = style_field(ctx.executor_keys, &theme.executor_style);
    let compose = match ctx.compose.filter(|c| !c.is_empty()) {
        Some(name) => {
            let truncated = truncate_text_for_display(name, max_length, suffix);
            format!(
                "{}{}",
                apply_style(" | ", &theme.prompt_style, theme.ansi),
                apply_style(&truncated, &theme.compose_style, theme.ansi)
            )
        }
        None => String::new(),
    };

    let tokens: [(&str, &str); 4] = [
        ("{username}", &username),
        ("{instance}", &instance),
        ("{compose}", &compose),
        ("{executor_keys}", &executor),
    ];

    let mut out = String::new();
    let mut rest = theme.prompt_template.as_str();
    while !rest.is_empty() {
        let next = tokens
            .iter()
            .filter_map(|&(pat, val)| rest.find(pat).map(|idx| (idx, pat.len(), val)))
            .min_by_key(|&(idx, _, _)| idx);
        match next {
            Some((idx, len, val)) => {
                if idx > 0 {
                    out.push_str(&apply_style_multiline(
                        &rest[..idx],
                        &theme.prompt_style,
                        theme.ansi,
                    ));
                }
                out.push_str(val);
                rest = &rest[idx + len..];
            }
            None => {
                out.push_str(&apply_style_multiline(
                    rest,
                    &theme.prompt_style,
                    theme.ansi,
                ));
                break;
            }
        }
    }
    out
}

fn format_result(theme: &ShellTheme, interactive: bool, ok: bool, message: &str) -> String {
    if !interactive {
        return message.to_owned();
    }
    let (icon, style) = if ok {
        (&theme.success_icon, &theme.success_style)
    } else {
        (&theme.failure_icon, &theme.failure_style)
    };
    let mut lines = message.split('\n');
    let first = lines.next().unwrap_or("");
    let header = if icon.is_empty() {
        format!(" {first}")
    } else {
        format!(" {icon} {first}")
    };
    let styled_header = apply_style(&header, style, theme.ansi);
    let rest: Vec<&str> = lines.collect();
    if rest.is_empty() {
        styled_header
    } else {
        format!("{styled_header}\n{}", rest.join("\n"))
    }
}

#[derive(Debug, Clone)]
struct ToolbarRun {
    text: String,
    sgr: String,
}

impl ToolbarRun {
    fn plain(text: impl Into<String>) -> Self {
        Self {
            text: text.into(),
            sgr: String::new(),
        }
    }

    fn colored(text: impl Into<String>, style: &str, ansi: bool) -> Self {
        Self {
            text: text.into(),
            sgr: if ansi {
                style_to_sgr(style)
            } else {
                String::new()
            },
        }
    }
}

fn runs_width(runs: &[ToolbarRun]) -> usize {
    runs.iter().map(|r| display_width(&r.text)).sum()
}

fn render_toolbar_row(runs: &[ToolbarRun], cols: usize, ansi: bool) -> String {
    let mut out = String::new();
    let mut used = 0usize;
    'outer: for run in runs {
        if ansi {
            out.push_str("\x1b[");
            out.push_str(TOOLBAR_BASE_SGR);
            if !run.sgr.is_empty() {
                out.push(';');
                out.push_str(&run.sgr);
            }
            out.push('m');
        }
        for c in run.text.chars() {
            let w = char_display_width(c);
            if used + w > cols {
                break 'outer;
            }
            out.push(c);
            used += w;
        }
    }
    if used < cols {
        if ansi {
            out.push_str(&format!("\x1b[{TOOLBAR_BASE_SGR}m"));
        }
        out.push_str(&" ".repeat(cols - used));
    }
    if ansi {
        out.push_str("\x1b[0m");
    }
    out
}

fn render_toolbar(
    theme: &ShellTheme,
    instance: &str,
    transport: &str,
    connected: bool,
    cols: usize,
) -> Vec<String> {
    if !theme.toolbar_enabled || cols == 0 {
        return Vec::new();
    }
    let suffix = if theme.unicode { "…" } else { "..." };
    let max_length = (cols / 4).max(4);
    let inst = truncate_text_for_display(instance, max_length, suffix);
    let trans = truncate_text_for_display(transport, max_length, suffix);

    let (status_word, status_style, icon) = if connected {
        ("connected", &theme.connected_style, &theme.connected_icon)
    } else {
        (
            "disconnected",
            &theme.disconnected_style,
            &theme.disconnected_icon,
        )
    };

    let left = vec![
        ToolbarRun::plain(" "),
        ToolbarRun::colored(inst, &format!("on {}", theme.instance_style), theme.ansi),
        ToolbarRun::colored(" | ", "on white", theme.ansi),
        ToolbarRun::colored(trans, &format!("on {}", theme.executor_style), theme.ansi),
    ];

    let mut right = vec![ToolbarRun::colored(
        status_word,
        &format!("on {status_style}"),
        theme.ansi,
    )];
    if !icon.is_empty() {
        right.push(ToolbarRun::plain(format!(" {icon}")));
    }
    right.push(ToolbarRun::plain(" "));

    let left_w = runs_width(&left);
    let right_w = runs_width(&right);
    if left_w + right_w + 1 <= cols {
        let mut runs = left;
        runs.push(ToolbarRun::plain(" ".repeat(cols - left_w - right_w)));
        runs.extend(right);
        vec![render_toolbar_row(&runs, cols, theme.ansi)]
    } else {
        let mut second = vec![ToolbarRun::plain(" ")];
        second.extend(right);
        vec![
            render_toolbar_row(&left, cols, theme.ansi),
            render_toolbar_row(&second, cols, theme.ansi),
        ]
    }
}

// --- Command preprocessing ---

#[derive(Debug, Clone, PartialEq, Eq)]
enum ShellAction {
    Empty,
    Exit,
    Clear,
    Complete(String),
    DeviceAction(String),
}

fn preprocess_line(raw: &str) -> ShellAction {
    let mut s = raw.trim();
    if s.is_empty() {
        return ShellAction::Empty;
    }
    if s == "mrsm" {
        return ShellAction::Empty;
    }
    if let Some(rest) = s.strip_prefix("mrsm")
        && rest.starts_with(char::is_whitespace)
    {
        s = rest.trim_start();
        if s.is_empty() {
            return ShellAction::Empty;
        }
    }
    match s {
        "exit" | "quit" => return ShellAction::Exit,
        "clear" => return ShellAction::Clear,
        "?" => return ShellAction::Complete(String::new()),
        _ => {}
    }
    if let Some(prefix) = s.strip_suffix('?') {
        return ShellAction::Complete(prefix.trim_start().to_owned());
    }
    if let Some(rest) = s.strip_prefix('?') {
        let target = rest.trim();
        return if target.is_empty() {
            ShellAction::Complete(String::new())
        } else {
            ShellAction::DeviceAction(format!("help {target}"))
        };
    }
    let tokens: Vec<&str> = s.split_whitespace().collect();
    if tokens.contains(&"--help") {
        let mut remaining: Vec<&str> = tokens.into_iter().filter(|&t| t != "--help").collect();
        if remaining.first() == Some(&"help") {
            remaining.remove(0);
        }
        return if remaining.is_empty() {
            ShellAction::DeviceAction("help".to_owned())
        } else {
            ShellAction::DeviceAction(format!("help {}", remaining.join(" ")))
        };
    }
    ShellAction::DeviceAction(s.to_owned())
}

// --- Reedline integrations ---

struct DevicePrompt(String);

impl Prompt for DevicePrompt {
    fn render_prompt_left(&self) -> Cow<'_, str> {
        Cow::Borrowed(&self.0)
    }
    fn render_prompt_right(&self) -> Cow<'_, str> {
        Cow::Borrowed("")
    }
    fn render_prompt_indicator(&self, _: PromptEditMode) -> Cow<'_, str> {
        Cow::Borrowed("")
    }
    fn render_prompt_multiline_indicator(&self) -> Cow<'_, str> {
        Cow::Borrowed("")
    }
    fn render_prompt_history_search_indicator(&self, search: PromptHistorySearch) -> Cow<'_, str> {
        Cow::Owned(
            DefaultPrompt::default()
                .render_prompt_history_search_indicator(search)
                .into_owned(),
        )
    }
    fn get_prompt_color(&self) -> ReedlineColor {
        ReedlineColor::Default
    }
    fn get_indicator_color(&self) -> ReedlineColor {
        ReedlineColor::Default
    }
    fn get_prompt_right_color(&self) -> ReedlineColor {
        ReedlineColor::Default
    }
}

struct DeviceToolbarHinter {
    theme: ShellTheme,
    instance: String,
    transport: String,
    connected: Arc<AtomicBool>,
}

impl Hinter for DeviceToolbarHinter {
    fn handle(
        &mut self,
        _line: &str,
        _pos: usize,
        _history: &dyn History,
        _use_ansi_coloring: bool,
        _cwd: &str,
    ) -> String {
        if !self.theme.toolbar_enabled {
            return String::new();
        }
        let (cols, rows) = terminal_size();
        let toolbar_rows = render_toolbar(
            &self.theme,
            &self.instance,
            &self.transport,
            self.connected.load(Ordering::Relaxed),
            cols,
        );
        if toolbar_rows.is_empty() || rows <= toolbar_rows.len() + 1 {
            return String::new();
        }
        let mut hint = String::from("\r\n");
        for (i, row) in toolbar_rows.iter().enumerate() {
            let target_row = rows - toolbar_rows.len() + i + 1;
            hint.push_str(&format!("\x1b[{target_row};1H{row}"));
        }
        hint
    }

    fn complete_hint(&self) -> String {
        String::new()
    }

    fn next_hint_token(&self) -> String {
        String::new()
    }
}

struct DeviceCompletions {
    config: Value,
    connected: Arc<AtomicBool>,
}

impl Completer for DeviceCompletions {
    fn complete(&mut self, line: &str, pos: usize) -> CompletionResult {
        let Some(prefix) = line.get(..pos) else {
            return CompletionResult::fresh(Vec::new());
        };
        let Ok(Value::Array(items)) =
            exchange(&self.config, json!({"op":"complete", "line":prefix}))
        else {
            self.connected.store(false, Ordering::Relaxed);
            return CompletionResult::fresh(Vec::new());
        };
        self.connected.store(true, Ordering::Relaxed);
        let suggestions = items
            .iter()
            .filter_map(Value::as_str)
            .filter(|name| !name.chars().any(char::is_control))
            .map(|name| Suggestion {
                value: name.to_owned(),
                span: Span::new(0, pos),
                append_whitespace: false,
                ..Default::default()
            })
            .collect::<Vec<_>>();
        CompletionResult::fresh(suggestions)
    }
}

fn query_completions(config: &Value, prefix: &str, connected: &AtomicBool) -> Result<Vec<String>> {
    match exchange(config, json!({"op":"complete", "line":prefix})) {
        Ok(Value::Array(items)) => {
            connected.store(true, Ordering::Relaxed);
            Ok(items
                .iter()
                .filter_map(Value::as_str)
                .filter(|name| !name.chars().any(char::is_control))
                .map(str::to_owned)
                .collect())
        }
        Ok(_) => {
            connected.store(true, Ordering::Relaxed);
            Err(error("Invalid device completion result."))
        }
        Err(e) => {
            connected.store(false, Ordering::Relaxed);
            Err(e)
        }
    }
}

pub(crate) fn run(config: &Value, instance: &str) -> Result<()> {
    let interactive = io::stdin().is_terminal() && io::stdout().is_terminal();
    let theme = ShellTheme::from_host(interactive)?;
    let username = config["username"]
        .as_str()
        .filter(|s| !s.is_empty())
        .unwrap_or("(no username)");
    let compose = compose_project_name(env_non_empty("MRSM__COMPOSE_CONFIG").as_deref());
    let transport = transport_label(config);
    let connected = Arc::new(AtomicBool::new(false));

    if interactive {
        println!("{}", render_banner(&theme, VERSION));
    }

    let mut editor = if interactive {
        let mut keys = default_emacs_keybindings();
        keys.add_binding(
            KeyModifiers::NONE,
            KeyCode::Tab,
            ReedlineEvent::UntilFound(vec![
                ReedlineEvent::Menu("actions".into()),
                ReedlineEvent::MenuNext,
            ]),
        );
        let history = build_history(theme.max_history, history_path());
        Some(
            Reedline::create()
                .with_ansi_colors(theme.ansi)
                .with_history(Box::new(history))
                .with_edit_mode(Box::new(Emacs::new(keys)))
                .with_completer(Box::new(DeviceCompletions {
                    config: config.clone(),
                    connected: connected.clone(),
                }))
                .with_hinter(Box::new(DeviceToolbarHinter {
                    theme: theme.clone(),
                    instance: instance.to_owned(),
                    transport: transport.clone(),
                    connected: connected.clone(),
                }))
                .with_quick_completions(true)
                .with_menu(ReedlineMenu::EngineCompleter(Box::new(
                    ColumnarMenu::default().with_name("actions"),
                ))),
        )
    } else {
        None
    };

    let mut lines = io::stdin().lines();
    loop {
        let line = if let Some(editor) = editor.as_mut() {
            let (cols, _) = terminal_size();
            let prompt = DevicePrompt(render_prompt(
                &theme,
                &PromptContext {
                    username,
                    instance,
                    compose: compose.as_deref(),
                    executor_keys: &theme.executor_keys,
                },
                cols,
            ));
            match editor.read_line(&prompt).map_err(error)? {
                Signal::Success(line) => line,
                Signal::CtrlD => break,
                _ => continue,
            }
        } else {
            let Some(line) = lines.next() else { break };
            line.map_err(error)?
        };

        match preprocess_line(&line) {
            ShellAction::Empty => continue,
            ShellAction::Exit => break,
            ShellAction::Clear => {
                if interactive && theme.ansi {
                    let mut out = io::stdout().lock();
                    let _ = out.write_all(b"\x1b[2J\x1b[H");
                    let _ = out.flush();
                }
            }
            ShellAction::Complete(prefix) => match query_completions(config, &prefix, &connected) {
                Ok(matches) => {
                    if matches.is_empty() {
                        if interactive {
                            let msg = if prefix.is_empty() {
                                "No matching actions.".to_owned()
                            } else {
                                format!("No matching actions for '{prefix}'.")
                            };
                            eprintln!("{}", format_result(&theme, true, false, &msg));
                        }
                    } else if interactive {
                        for name in matches {
                            println!(
                                "  {}",
                                apply_style(&name, &theme.instance_style, theme.ansi)
                            );
                        }
                    } else {
                        for name in matches {
                            println!("{name}");
                        }
                    }
                }
                Err(e) => {
                    eprintln!(
                        "{}",
                        format_result(&theme, interactive, false, &e.to_string())
                    );
                }
            },
            ShellAction::DeviceAction(action_line) => {
                match exchange(config, json!({"op":"action","line":action_line})) {
                    Ok(Value::String(msg)) => {
                        connected.store(true, Ordering::Relaxed);
                        println!("{}", format_result(&theme, interactive, true, &msg));
                    }
                    Ok(_) => {
                        connected.store(true, Ordering::Relaxed);
                        eprintln!(
                            "{}",
                            format_result(
                                &theme,
                                interactive,
                                false,
                                "Invalid device action result. Inspect the device before retrying.",
                            )
                        );
                    }
                    Err(e) => {
                        let msg = e.to_string();
                        if msg.contains("Timed out")
                            || msg.contains("could not open")
                            || msg.contains("transport closed")
                            || msg.contains("unavailable")
                        {
                            connected.store(false, Ordering::Relaxed);
                        }
                        eprintln!("{}", format_result(&theme, interactive, false, &msg));
                    }
                }
            }
        }
    }

    if let Some(editor) = editor.as_mut() {
        let _ = editor.sync_history();
    }
    if interactive {
        print!("{}", render_close_message(&theme));
        let _ = io::stdout().flush();
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::OsStr;

    fn sample_theme(ansi: bool, unicode: bool) -> ShellTheme {
        ShellTheme {
            ansi,
            unicode,
            prompt_template: if unicode {
                "\n [ {username}@{instance}{compose} ] ➤ ".into()
            } else {
                "\n [ {username}@{instance}{compose} ] > ".into()
            },
            prompt_style: "green".into(),
            username_style: "white".into(),
            instance_style: "cyan".into(),
            compose_style: "bright_magenta".into(),
            executor_style: "yellow".into(),
            intro_style: "bold bright_blue".into(),
            close_message: if unicode {
                " 👋 Thank you for using Meerschaum! ".into()
            } else {
                "Thank you for using Meerschaum!".into()
            },
            close_style: "bright_blue".into(),
            success_icon: if unicode { "🎉".into() } else { "+".into() },
            success_style: "bold bright_green".into(),
            failure_icon: if unicode { "💢".into() } else { "-".into() },
            failure_style: "bold red".into(),
            connected_icon: if unicode {
                "🟢".into()
            } else {
                String::new()
            },
            connected_style: "green".into(),
            disconnected_icon: if unicode {
                "🔴".into()
            } else {
                String::new()
            },
            disconnected_style: "red".into(),
            toolbar_enabled: true,
            max_history: 1000,
            executor_keys: "local".into(),
        }
    }

    fn strip_sgr(s: &str) -> String {
        let mut out = String::new();
        let bytes = s.as_bytes();
        let mut i = 0;
        while i < bytes.len() {
            if bytes[i] == 0x1b && i + 1 < bytes.len() && bytes[i + 1] == b'[' {
                i += 2;
                while i < bytes.len() && bytes[i] != b'm' {
                    i += 1;
                }
                if i < bytes.len() {
                    i += 1;
                }
            } else {
                let ch = s[i..].chars().next().unwrap();
                out.push(ch);
                i += ch.len_utf8();
            }
        }
        out
    }

    #[test]
    fn style_parser_handles_named_attrs_hex_and_bg() {
        assert_eq!(style_to_sgr("bold bright_blue"), "1;94");
        assert_eq!(style_to_sgr("bold bright blue"), "1;94");
        assert_eq!(style_to_sgr("green"), "32");
        assert_eq!(style_to_sgr("cyan"), "36");
        assert_eq!(style_to_sgr("white"), "37");
        assert_eq!(style_to_sgr("bold bright_green"), "1;92");
        assert_eq!(style_to_sgr("bold red"), "1;31");
        assert_eq!(style_to_sgr("on cyan"), "46");
        assert_eq!(style_to_sgr("on bright_magenta"), "105");
        assert_eq!(style_to_sgr("#d177a4"), "38;2;209;119;164");
        assert_eq!(style_to_sgr("dim on #010203"), "2;48;2;1;2;3");
        assert_eq!(style_to_sgr("none"), "");
        assert_eq!(style_to_sgr(""), "");
    }

    #[test]
    fn no_color_env_follows_spec() {
        assert!(!no_color_env(None));
        assert!(!no_color_env(Some(OsStr::new(""))));
        assert!(no_color_env(Some(OsStr::new("1"))));
        assert!(no_color_env(Some(OsStr::new("0"))));
    }

    #[test]
    fn banner_and_close_message_honor_ansi_and_charset() {
        let styled = render_banner(&sample_theme(true, true), "0.2.1");
        assert!(styled.contains("\x1b[1;94m"));
        assert!(styled.contains("MicroMeerschaum"));
        assert!(styled.contains("v0.2.1"));

        let plain = render_banner(&sample_theme(false, false), "0.2.1");
        assert!(!plain.contains('\x1b'));
        assert!(plain.contains("MicroMeerschaum"));
        assert!(plain.contains("v0.2.1"));
        assert!(plain.is_ascii());

        let close_styled = render_close_message(&sample_theme(true, true));
        assert!(close_styled.contains("\x1b[94m"));
        assert!(close_styled.contains("Thank you for using Meerschaum!"));

        let close_plain = render_close_message(&sample_theme(false, false));
        assert!(!close_plain.contains('\x1b'));
        assert!(close_plain.contains("Thank you for using Meerschaum!"));
    }

    #[test]
    fn prompt_colors_fields_and_truncates_narrow_terminals() {
        let theme = sample_theme(true, true);
        let ctx = PromptContext {
            username: "(no username)",
            instance: "micro:bench",
            compose: None,
            executor_keys: "local",
        };
        let rendered = render_prompt(&theme, &ctx, 80);
        assert!(rendered.contains("\x1b[32m [ \x1b[0m"));
        assert!(rendered.contains("\x1b[37m(no username)\x1b[0m"));
        assert!(rendered.contains("\x1b[36mmicro:bench\x1b[0m"));
        assert_eq!(strip_sgr(&rendered), "\n [ (no username)@micro:bench ] ➤ ");

        // Narrow terminal (40 cols -> max field width 10) truncates before coloring.
        let narrow = render_prompt(
            &theme,
            &PromptContext {
                username: "very_long_username",
                instance: "micro:extremely_long_label",
                compose: Some("my_compose_project"),
                executor_keys: "local",
            },
            40,
        );
        assert!(narrow.contains("\x1b[37mvery_long…\x1b[0m"));
        assert!(narrow.contains("\x1b[36mmicro:ext…\x1b[0m"));
        assert!(narrow.contains("\x1b[95mmy_compos…\x1b[0m"));

        // Plain mode emits zero ANSI escapes.
        let plain = render_prompt(&sample_theme(false, false), &ctx, 80);
        assert!(!plain.contains('\x1b'));
        assert_eq!(plain, "\n [ (no username)@micro:bench ] > ");
    }

    #[test]
    fn result_formatting_distinguishes_interactive_plain_and_piped() {
        let styled = format_result(&sample_theme(true, true), true, true, "Ran show pipes");
        assert_eq!(styled, "\x1b[1;92m 🎉 Ran show pipes\x1b[0m");

        let failed = format_result(&sample_theme(true, true), true, false, "Refused");
        assert_eq!(failed, "\x1b[1;31m 💢 Refused\x1b[0m");

        let no_ansi = format_result(&sample_theme(false, false), true, true, "Ran show pipes");
        assert_eq!(no_ansi, " + Ran show pipes");

        let piped = format_result(&sample_theme(true, true), false, true, "Ran show pipes");
        assert_eq!(piped, "Ran show pipes");
    }

    #[test]
    fn toolbar_renders_status_transport_and_wraps_narrow() {
        let rows = render_toolbar(&sample_theme(true, true), "micro:bench", "serial", true, 80);
        assert_eq!(rows.len(), 1);
        let plain = strip_sgr(&rows[0]);
        assert!(plain.starts_with(" micro:bench | serial "), "{plain:?}");
        assert!(plain.trim_end().ends_with("connected 🟢"), "{plain:?}");
        assert_eq!(display_width(&plain), 80);
        assert!(rows[0].contains("\x1b[0;38;5;16;7;46m"));
        assert!(rows[0].contains("\x1b[0;38;5;16;7;42m"));

        let disconnected =
            render_toolbar(&sample_theme(false, false), "micro:ble1", "ble", false, 60);
        assert_eq!(disconnected.len(), 1);
        assert!(!disconnected[0].contains('\x1b'));
        assert!(disconnected[0].contains("micro:ble1 | ble"));
        assert!(disconnected[0].contains("disconnected"));

        let wrapped = render_toolbar(
            &sample_theme(true, true),
            "micro:bench",
            "serial",
            false,
            24,
        );
        assert_eq!(wrapped.len(), 2);
        assert!(strip_sgr(&wrapped[0]).contains("micro…"));
        assert!(strip_sgr(&wrapped[0]).contains("serial"));
        assert!(strip_sgr(&wrapped[1]).contains("disconnected"));
    }

    #[test]
    fn preprocess_handles_conveniences_and_mrsm_stripping() {
        assert_eq!(preprocess_line("   "), ShellAction::Empty);
        assert_eq!(preprocess_line("mrsm"), ShellAction::Empty);
        assert_eq!(preprocess_line("mrsm   "), ShellAction::Empty);
        assert_eq!(preprocess_line("exit"), ShellAction::Exit);
        assert_eq!(preprocess_line("mrsm quit"), ShellAction::Exit);
        assert_eq!(preprocess_line("clear"), ShellAction::Clear);
        assert_eq!(preprocess_line("mrsm clear"), ShellAction::Clear);
        assert_eq!(preprocess_line("?"), ShellAction::Complete(String::new()));
        assert_eq!(preprocess_line("sh?"), ShellAction::Complete("sh".into()));
        assert_eq!(
            preprocess_line("mrsm show?"),
            ShellAction::Complete("show".into())
        );
        assert_eq!(
            preprocess_line("?show pipes"),
            ShellAction::DeviceAction("help show pipes".into())
        );
        assert_eq!(
            preprocess_line("--help"),
            ShellAction::DeviceAction("help".into())
        );
        assert_eq!(
            preprocess_line("show pipes --help"),
            ShellAction::DeviceAction("help show pipes".into())
        );
        assert_eq!(
            preprocess_line("mrsm show pipes --help"),
            ShellAction::DeviceAction("help show pipes".into())
        );
        assert_eq!(
            preprocess_line("help --help"),
            ShellAction::DeviceAction("help".into())
        );
        assert_eq!(
            preprocess_line("mrsm show pipes"),
            ShellAction::DeviceAction("show pipes".into())
        );
        assert_eq!(
            preprocess_line("mrsm_custom arg"),
            ShellAction::DeviceAction("mrsm_custom arg".into())
        );
    }

    #[test]
    fn history_path_honors_xdg_state_home_and_persists() {
        let p = history_path_from_env(Some("/tmp/state"), None, Some("/home/u"), None).unwrap();
        assert_eq!(p, PathBuf::from("/tmp/state/micromeerschaum/shell_history"));

        let fallback = history_path_from_env(None, None, Some("/home/u"), None).unwrap();
        assert_eq!(
            fallback,
            PathBuf::from("/home/u/.local/state/micromeerschaum/shell_history")
        );

        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("micromeerschaum").join("shell_history");
        {
            let mut h = build_history(2, Some(file.clone()));
            let _ = h.save(reedline::HistoryItem::from_command_line("first"));
            let _ = h.save(reedline::HistoryItem::from_command_line("second"));
            let _ = h.save(reedline::HistoryItem::from_command_line("third"));
            h.sync().unwrap();
        }
        let h2 = build_history(2, Some(file));
        let items = h2
            .search(reedline::SearchQuery::everything(
                reedline::SearchDirection::Forward,
                None,
            ))
            .unwrap();
        let cmds: Vec<&str> = items.iter().map(|i| i.command_line.as_str()).collect();
        assert_eq!(cmds, vec!["second", "third"]);
    }
}
