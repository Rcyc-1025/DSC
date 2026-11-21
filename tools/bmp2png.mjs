// 24bpp bottom-up BMP -> PNG（仅用 Node 标准库；本机无 PIL，界面截图靠它转格式）
import fs from 'node:fs';
import zlib from 'node:zlib';

function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([len, body, crc]);
}

const [, , src, dst] = process.argv;
const b = fs.readFileSync(src);
const off = b.readUInt32LE(10);
const w = b.readInt32LE(18);
const h = b.readInt32LE(22);
const bpp = b.readUInt16LE(28);
if (bpp !== 24 && bpp !== 32) throw new Error('only 24/32bpp supported, got ' + bpp);
const px = bpp / 8;
const rowBytes = Math.floor((bpp * w + 31) / 32) * 4;
const raw = Buffer.alloc((w * 3 + 1) * h);
let o = 0;
for (let y = h - 1; y >= 0; y--) {
  const rowOff = off + y * rowBytes;
  raw[o++] = 0;
  for (let x = 0; x < w; x++) {
    const i = rowOff + x * px;
    raw[o++] = b[i + 2];
    raw[o++] = b[i + 1];
    raw[o++] = b[i];
  }
}
const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(w, 0);
ihdr.writeUInt32BE(h, 4);
ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', ihdr),
  chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
  chunk('IEND', Buffer.alloc(0)),
]);
fs.writeFileSync(dst, png);
console.log('png ' + w + 'x' + h + ' -> ' + dst + ' (' + png.length + ' bytes)');
