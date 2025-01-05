# kjuv
libuv integration for capnproto

##  TODO
- cmake build system
- deal with spurious level-triggered IO events as documented here: https://docs.libuv.org/en/v1.x/poll.html#c.uv_poll_start
- investigate uv_stream_t and friends
- factor out uv_run calls from poll/wait
- signal handling?
