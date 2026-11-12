# Records demo.gif, a run of the bench tests, with VHS (https://github.com/charmbracelet/vhs).
#
#     vhs demo.tape
#
# Record it on the computer of the bench, because the tests need the bus and the P 1535. The
# recording clones the repository from GitHub into a temporary directory, builds the Docker image of
# the server from that clone, and runs it the way the Bench Tests workflow does, so the server
# matches the tests. Nothing else may use port 61447 or 62281, and no Bench Tests run may use the
# bench at the same time. The tests reset that server and switch the drives off. The slow commands
# run hidden, so the GIF shows their output but not the wait. The first build takes many minutes.

Output demo.gif

Require git
Require python3
Require docker
Require curl

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

# The server, Motion Master: built from the clone and started the way the Bench Tests workflow
# starts it, with the capabilities SOEM and the RT loop need. The build and the wait for the server
# to answer run hidden, so the GIF shows the commands but not the wait.
Type "cd motion-master" Sleep 500ms Enter
Hide
Type "docker rm -f motion-master-bench; clear" Enter
Wait@60s
Show
Type "docker build -q -t motion-master-bench ." Sleep 500ms Enter
Hide
Wait@3600s
Show
Sleep 1s
# A command with no ">", which Wait would take for the prompt if the long line wrapped there.
Type "docker run -d --rm --name motion-master-bench --network host --cap-add NET_RAW --cap-add NET_ADMIN --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 motion-master-bench" Sleep 500ms Enter
Type "until curl -ksf -o /dev/null https://127.0.0.1:61447/api/version; do sleep 1; done" Sleep 500ms Enter
Hide
Wait@300s
Show
Sleep 1s

Type "cd hil/bench" Sleep 500ms Enter
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

Hide
Type "docker stop motion-master-bench" Enter
Wait@60s
