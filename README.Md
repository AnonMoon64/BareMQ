# BareMQ

BareMQ is a small, single-source MQTT 3.1.1 library written in C. It provides a compact client implementation
and a lightweight Windows IOCP-based server acceptor. The project is intentionally minimal  the API is
exposed via include/baremq.h and the reference implementation lives in src/baremq.c.

This README focuses on building and using the library on Windows with MinGW (the code is also portable to
POSIX platforms with minor changes).

## Build (Windows, MinGW)

The code uses Winsock2 and some Windows-specific APIs. When compiling with MinGW, link with ws2_32
and (optionally) mswsock to resolve AcceptEx if needed.

PowerShell examples (adjust the compiler path if necessary):

`powershell
# Compile object file
C:\MinGW\bin\x86_64-w64-mingw32-gcc.exe -Iinclude -c src/baremq.c -o src/baremq.o

# Build standalone executable (simple example)
C:\MinGW\bin\x86_64-w64-mingw32-gcc.exe -Iinclude src/baremq.c -o baremq.exe -lws2_32 -lmswsock -mconsole
`

Notes:
- If you only want to compile the library into your project, compile src/baremq.c and link the resulting object
  into your application, remembering to add -lws2_32 on Windows builds.
- On POSIX systems you can replace Winsock-specific code with the POSIX sockets and link with -pthread as
  appropriate.

## Minimal Usage (client)

Include the header and call the provided API. A minimal flow looks like:

- aremq_init() to create a client instance
- aremq_connect() to establish a connection
- aremq_sub() / aremq_send() for subscriptions and publishes
- aremq_recv() with a callback to process incoming messages
- aremq_disconnect() and aremq_free() to clean up

See the source for examples and usage patterns by searching for aremq_init and friends.

## Recent Fixes

While building the project on Windows, a couple of compile issues were fixed in src/baremq.c:

- A missing global g_listen_socket was declared and initialized to INVALID_SOCKET to fix
  "g_listen_socket undeclared" compile errors.
- A forward declaration for aremq_event_loop was added to avoid implicit-declaration warnings when the
  function is referenced before its definition.

If you encounter other compiler warnings or platform-specific issues, open an issue or send a PR.

## Contributing

Contributions are welcome. Keep changes small and focused. When submitting patches:

- Add a short description in the PR.
- Avoid unnecessary dependencies.
- Run a native build on the target platform (Windows, Linux, macOS, Android) if possible.

## License

This project is provided under the MIT License. See LICENSE for details.

---
Updated README to include clear Windows build instructions and notes about small fixes applied to the source.
