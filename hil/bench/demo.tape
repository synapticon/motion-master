# Records demo.gif, a run of the bench tests, with VHS (https://github.com/charmbracelet/vhs).
#
#     vhs demo.tape
#
# Record it on the computer of the bench, because the tests need the bus and the P 1535. A Motion
# Master server must run on that computer, on the default ports, with nothing else connected to it.
# The tests reset that server and switch the drives off. The recording clones the repository from
# GitHub into a temporary directory. The slow commands run hidden, so the GIF shows their output
# but not the wait.

Output demo.gif

Require git
Require python3

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

Type "cd motion-master/hil/bench" Sleep 500ms Enter
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

Type "grep -E '^(adapter|productName|folder)' bench.description.toml" Sleep 500ms Enter
Sleep 3s

# The run starts with a power cycle and takes minutes, so the GIF shows the end of the output.
Type "pytest" Sleep 500ms Enter
Hide
Wait@900s
Show
Sleep 8s
