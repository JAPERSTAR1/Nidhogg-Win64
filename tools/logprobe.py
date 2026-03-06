path = r"C:\Users\puy12\Downloads\g\nidhogg-port\out.log"
with open(path, "rb") as f:
    head = f.read(4096)
print(repr(head[:200]))
print("hle in head:", b"[hle" in head)
print("nul in head:", head.count(b"\x00"))
print("crlf:", head.count(b"\r\n"), "lf:", head.count(b"\n"))
