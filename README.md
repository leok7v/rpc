# rpc
Simple Windows RPC streaming server client

    rpc.exe server

    rpc.exe client [--shutdown]

rpc client is capable of running server inside client process.

No admin/system elevated privileges required on both sides 
as long as both client and server are running as non-elevated 
processes in a user account.

    rpc\bin\Release>rpc client
    rpc\src\client.c(25): [01048] roundtrip client.set() 7.717 microseconds
    rpc\src\client.c(26): [01048] roundtrip client.get("foo")="Goodbye Universe"
    rpc\src\server.c(58): [13672] start start
    rpc\src\server.c(66): [13672] stop stop
    rpc\src\client.c(72): [01048] streaming latency[0]=49.9 us
    rpc\src\client.c(72): [01048] streaming latency[1]=16.6 us
    rpc\src\rpc.c(74): [13672] remove_client_at removing client[0] pid=9272 running=0

