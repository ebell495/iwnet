cd ./build/src/ws/tests/ws_client_test3
file ./ws_client_test3
#set args -c

set follow-fork-mode parent
set detach-on-fork on
set print elements 4096

define lb
    set breakpoint pending on
    source ~/Projects/softmotions/iwnet/.breakpoints
    set breakpoint pending auto
    echo breakpoints loaded\n
end

define sb
    save breakpoints ~/Projects/softmotions/iwnet/.breakpoints
    echo breakpoints saved\n
end
