# https://stackoverflow.com/questions/40033311/how-to-debug-programs-with-sudo-in-vscode
#
# vim /etc/sudoers.d/gdb
# ALL ALL=(ALL) NOPASSWD:/usr/bin/gdb

sudo /usr/bin/gdb "$@"
