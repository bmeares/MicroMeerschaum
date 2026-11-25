// SPDX-License-Identifier: Apache-2.0
use crate::device::{error, exchange};
use meerschaum_plugin::prelude::*;
use reedline::{
    ColumnarMenu, Completer, CompletionResult, DefaultPrompt, Emacs, KeyCode, KeyModifiers,
    MenuBuilder, Prompt, PromptEditMode, PromptHistorySearch, Reedline, ReedlineEvent,
    ReedlineMenu, Signal, Span, Suggestion, default_emacs_keybindings,
};
use std::{
    borrow::Cow,
    io::{self, IsTerminal},
};

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
}

struct DeviceCompletions(Value);

impl Completer for DeviceCompletions {
    fn complete(&mut self, line: &str, pos: usize) -> CompletionResult {
        let Some(prefix) = line.get(..pos) else {
            return CompletionResult::fresh(Vec::new());
        };
        let Ok(Value::Array(items)) = exchange(&self.0, json!({"op":"complete", "line":prefix}))
        else {
            return CompletionResult::fresh(Vec::new());
        };
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

pub(crate) fn run(config: &Value, instance: &str) -> Result<()> {
    let interactive = io::stdin().is_terminal() && io::stdout().is_terminal();
    let charset = if host::config(&["formatting", "unicode"])? == false {
        "ascii"
    } else {
        "unicode"
    };
    let setting = |keys: &[&str], fallback: &str| -> Result<String> {
        Ok(host::config(keys)?.as_str().unwrap_or(fallback).to_owned())
    };
    let prompt = DevicePrompt(
        setting(&["shell", charset, "prompt"], "[ {instance} ] > ")?
            .replace(
                "{username}",
                config["username"].as_str().unwrap_or("(no username)"),
            )
            .replace("{compose}", "")
            .replace("{executor_keys}", "local")
            .replace("{instance}", instance),
    );
    let success = setting(&["formatting", "success", charset, "icon"], "+")?;
    let failure = setting(&["formatting", "failure", charset, "icon"], "-")?;
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
        Some(
            Reedline::create()
                .with_ansi_colors(
                    host::config(&["formatting", "ansi"])? != false
                        && std::env::var_os("NO_COLOR").is_none_or(|v| v.is_empty()),
                )
                .with_edit_mode(Box::new(Emacs::new(keys)))
                .with_completer(Box::new(DeviceCompletions(config.clone())))
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
            match editor.read_line(&prompt).map_err(error)? {
                Signal::Success(line) => line,
                Signal::CtrlD => break,
                _ => continue,
            }
        } else {
            let Some(line) = lines.next() else { break };
            line.map_err(error)?
        };
        match line.trim() {
            "" => continue,
            "exit" | "quit" => break,
            _ => {}
        }
        match exchange(config, json!({"op":"action","line":line})) {
            Ok(Value::String(msg)) if interactive => println!(" {success} {msg}"),
            Ok(Value::String(msg)) => println!("{msg}"),
            Ok(_) => eprintln!("Invalid device action result. Inspect the device before retrying."),
            Err(e) if interactive => eprintln!(" {failure} {e}"),
            Err(e) => eprintln!("{e}"),
        }
    }
    Ok(())
}
