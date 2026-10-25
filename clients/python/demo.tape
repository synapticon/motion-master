# Records demo.gif, the first run of the Python client, with VHS (https://github.com/charmbracelet/vhs).
#
#     vhs demo.tape
#
# The recording clones the repository from GitHub into a temporary directory and starts the
# installed Motion Master release, which must be on PATH as motion-master. Nothing must listen on
# port 61447 or 62281. The slow commands run hidden, so the GIF shows their output but not the wait.

Output demo.gif

Require git
Require python
Require nano
Require motion-master

Set Shell bash
Set FontSize 16
Set Width 1280
Set Height 720
Set Padding 20
Set TypingSpeed 40ms
Set Theme "Catppuccin Mocha"

Hide
Type "cd $(mktemp -d) && clear" Enter
Show

Type "git clone https://github.com/synapticon/motion-master.git" Sleep 500ms Enter
Hide
Wait@300s
Show
Sleep 1s

Type "cd motion-master/clients/python" Sleep 500ms Enter
Sleep 500ms

Type "source setup.sh" Sleep 500ms Enter
Hide
Wait@300s
Show
Sleep 2s

# The pip output fills the screen, so clear it before the next step.
Hide
Type "clear" Enter
Show

Type "cp config.example.toml config.toml" Sleep 500ms Enter
Sleep 500ms

# Find the example MAC address in nano and replace it with the name of the adapter.
Type "nano config.toml" Sleep 500ms Enter
Sleep 1s
Ctrl+W
Sleep 300ms
Type "aa:bb"
Sleep 300ms
Enter
Sleep 800ms
Delete 17
Sleep 300ms
Type "enp0s13f0u1u2"
Sleep 1s
Ctrl+S
Sleep 500ms
Ctrl+X
Sleep 500ms

Type "grep '^adapter' config.toml" Sleep 500ms Enter
Sleep 1.5s

# The startup log prints after the prompt, so Enter gives the next command a clean prompt.
Type "motion-master &" Sleep 500ms Enter
Sleep 3s
Enter
Sleep 500ms

Type "python examples/server-info.py" Sleep 500ms Enter
Wait@30s
Sleep 6s

Hide
Type "kill %1 && wait" Enter
Wait@30s
