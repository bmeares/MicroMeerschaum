# Share device actions across entry points

A node exposes named actions through one dispatcher. An application can call an action directly, a MicroPython user can enter a line-oriented shell from the REPL and return to Python, and a native application can attach its existing console. Help and completion describe the same actions that the dispatcher can run. Each submitted line invokes one action.

Ship `help`, `show actions`, `show pipes`, `sync pipes` and `copy pipes` so operators can work with configured Pipes before adding an application plugin. Applications can register their own actions and keep their existing command text and replies. Each remote entry point explicitly authorizes the actions available to its caller. An application can add a local action without granting remote access to it.
