// Emscripten JS library of the browser build (src/CMakeLists.txt, --js-library): the game's file reads that need
// bytes not here yet suspend the engine (JSPI) while the page fetches them, instead of a synchronous request that holds
// the page — on a thin link, for seconds. A file the page mounted streamed says so through its node,
// `node.orthrosStreamed` = { missing(position, length), ensure(position, length): Promise } (src/ports/goldsrc/files.js,
// asyncReads, in Orthros); any other file reads as Emscripten's own fd_read / fd_pread do. Called only under main() — a promising
// export: the game reads its files from its own thread, never from a browser callback.
addToLibrary({
  // (the bytes a read asks for, from its iovecs: { iov_base, iov_len } pairs of 32-bit words)
  $orthrosReadWait__deps: ['$FS'],
  $orthrosReadWait: (stream, position, iov, iovcnt) => {
    var streamed = stream && stream.node && stream.node.orthrosStreamed;
    if (!streamed || typeof position != 'number') return null;
    var length = 0;
    for (var i = 0; i < iovcnt; i++) length += HEAPU32[(iov + i * 8 + 4) >> 2];
    return length > 0 && streamed.missing(position, length) ? streamed.ensure(position, length) : null;
  },
  // (the read itself, now that its bytes are here: Emscripten's, its errors as an errno)
  $orthrosReadv__deps: ['$doReadv', '$SYSCALLS', '$FS'],
  $orthrosReadv: (fd, iov, iovcnt, offset, pnum) => {
    try {
      var num = doReadv(SYSCALLS.getStreamFromFD(fd), iov, iovcnt, offset);
      HEAPU32[pnum >> 2] = num;
      return 0;
    } catch (e) {
      if (typeof FS == 'undefined' || e.name !== 'ErrnoError') throw e;
      return e.errno;
    }
  },
  $orthrosRead__deps: ['$orthrosReadWait', '$orthrosReadv', '$SYSCALLS'],
  $orthrosRead: (fd, iov, iovcnt, offset, pnum) => {
    var wait = null;
    try {
      var stream = SYSCALLS.getStreamFromFD(fd);
      wait = orthrosReadWait(stream, typeof offset == 'undefined' ? stream.position : offset, iov, iovcnt);
    } catch (e) { /* (a bad descriptor: the read reports it) */ }
    // (a fetch that failed: the read tries once more its own way — synchronously — and reports an error if it must)
    if (wait) return wait.then(() => orthrosReadv(fd, iov, iovcnt, offset, pnum), () => orthrosReadv(fd, iov, iovcnt, offset, pnum));
    return orthrosReadv(fd, iov, iovcnt, offset, pnum);
  },
  fd_read__async: true,
  fd_read__deps: ['$orthrosRead'],
  fd_read: (fd, iov, iovcnt, pnum) => orthrosRead(fd, iov, iovcnt, undefined, pnum),
  fd_pread__async: true,
  fd_pread__i53abi: true,
  fd_pread__deps: ['$orthrosRead'],
  fd_pread: (fd, iov, iovcnt, offset, pnum) => (isNaN(offset) ? 61 /* EOVERFLOW */ : orthrosRead(fd, iov, iovcnt, offset, pnum)),
});
