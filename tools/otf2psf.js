#!/usr/bin/env node
/**
 * otf2psf.js — Rasterize an OTF/TTF font into a PSF2 bitmap font.
 *
 * Usage:  node tools/otf2psf.js <input.otf> <output.psf> [pixel_height] [cell_width]
 *
 * Default pixel_height = 16, cell_width = 8.
 * Generates 256 ASCII glyphs (0-255).
 *
 * The OTF outlines are rasterized to 1-bit bitmaps packed MSB-first,
 * one byte per row (for cell_width <= 8).
 */

const fs   = require('fs');
const path = require('path');
const opentype = require('opentype.js');

/* ── CLI ─────────────────────────────────────────────────────────── */

const args = process.argv.slice(2);
if (args.length < 2) {
    console.error('Usage: node otf2psf.js <input.otf> <output.psf> [height=16] [width=8]');
    process.exit(1);
}

const inputPath  = args[0];
const outputPath = args[1];
const CELL_H     = parseInt(args[2] || '16', 10);
const CELL_W     = parseInt(args[3] || '8', 10);
const GLYPH_COUNT = 256;

if (CELL_W > 8) {
    console.error('Only cell_width <= 8 supported (1 byte per row).');
    process.exit(1);
}

/* ── Render one glyph to 1-bit bitmap ───────────────────────────── */

function renderGlyph(font, charCode, cellW, cellH, ssBaseline, ssFontSize) {
    const glyph = font.charToGlyph(String.fromCharCode(charCode));
    if (!glyph || (glyph.index === 0 && charCode !== 0)) {
        return new Uint8Array(cellH);
    }

    const SS = 4;
    const ssW = cellW * SS;
    const ssH = cellH * SS;
    const ssScale = ssFontSize / font.unitsPerEm;

    /* Get glyph advance for centering */
    const advance = (glyph.advanceWidth || font.unitsPerEm * 0.5) * ssScale;
    const ssOffsetX = Math.round((ssW - advance) / 2);

    /* Render glyph path into supersampled buffer */
    const buf = new Float32Array(ssW * ssH);
    const p = glyph.getPath(ssOffsetX, ssBaseline, ssFontSize);
    rasterizePath(p.commands, buf, ssW, ssH);

    /* Downsample to cell size with threshold */
    const bitmap = new Uint8Array(cellH);
    for (let row = 0; row < cellH; row++) {
        let byte = 0;
        for (let col = 0; col < cellW; col++) {
            let sum = 0;
            for (let sy = 0; sy < SS; sy++) {
                for (let sx = 0; sx < SS; sx++) {
                    const px = col * SS + sx;
                    const py = row * SS + sy;
                    if (px < ssW && py < ssH) {
                        sum += buf[py * ssW + px];
                    }
                }
            }
            const avg = sum / (SS * SS);
            if (avg > 0.35) {
                byte |= (0x80 >> col);
            }
        }
        bitmap[row] = byte;
    }

    return bitmap;
}

/* ── Simple scanline rasterizer for OpenType path commands ───────── */

function rasterizePath(commands, buf, w, h) {
    /* Collect line segments from the path */
    const edges = [];
    let cx = 0, cy = 0;
    let startX = 0, startY = 0;

    for (const cmd of commands) {
        switch (cmd.type) {
            case 'M':
                cx = cmd.x; cy = cmd.y;
                startX = cx; startY = cy;
                break;
            case 'L':
                edges.push({ x0: cx, y0: cy, x1: cmd.x, y1: cmd.y });
                cx = cmd.x; cy = cmd.y;
                break;
            case 'Q': {
                /* Flatten quadratic bezier */
                const steps = 8;
                for (let i = 0; i < steps; i++) {
                    const t0 = i / steps;
                    const t1 = (i + 1) / steps;
                    const ax = qbez(cx, cmd.x1, cmd.x, t0);
                    const ay = qbez(cy, cmd.y1, cmd.y, t0);
                    const bx = qbez(cx, cmd.x1, cmd.x, t1);
                    const by = qbez(cy, cmd.y1, cmd.y, t1);
                    edges.push({ x0: ax, y0: ay, x1: bx, y1: by });
                }
                cx = cmd.x; cy = cmd.y;
                break;
            }
            case 'C': {
                /* Flatten cubic bezier */
                const steps = 12;
                for (let i = 0; i < steps; i++) {
                    const t0 = i / steps;
                    const t1 = (i + 1) / steps;
                    const ax = cbez(cx, cmd.x1, cmd.x2, cmd.x, t0);
                    const ay = cbez(cy, cmd.y1, cmd.y2, cmd.y, t0);
                    const bx = cbez(cx, cmd.x1, cmd.x2, cmd.x, t1);
                    const by = cbez(cy, cmd.y1, cmd.y2, cmd.y, t1);
                    edges.push({ x0: ax, y0: ay, x1: bx, y1: by });
                }
                cx = cmd.x; cy = cmd.y;
                break;
            }
            case 'Z':
                if (cx !== startX || cy !== startY) {
                    edges.push({ x0: cx, y0: cy, x1: startX, y1: startY });
                }
                cx = startX; cy = startY;
                break;
        }
    }

    /* Scanline fill (even-odd rule) */
    for (let y = 0; y < h; y++) {
        const scanY = y + 0.5;
        const crossings = [];

        for (const e of edges) {
            const { x0, y0, x1, y1 } = e;
            if ((y0 <= scanY && y1 > scanY) || (y1 <= scanY && y0 > scanY)) {
                const t = (scanY - y0) / (y1 - y0);
                crossings.push(x0 + t * (x1 - x0));
            }
        }

        crossings.sort((a, b) => a - b);

        for (let i = 0; i + 1 < crossings.length; i += 2) {
            const xStart = Math.max(0, Math.floor(crossings[i]));
            const xEnd   = Math.min(w - 1, Math.ceil(crossings[i + 1]));
            for (let x = xStart; x <= xEnd; x++) {
                buf[y * w + x] = 1.0;
            }
        }
    }
}

function qbez(p0, p1, p2, t) {
    const mt = 1 - t;
    return mt * mt * p0 + 2 * mt * t * p1 + t * t * p2;
}

function cbez(p0, p1, p2, p3, t) {
    const mt = 1 - t;
    return mt*mt*mt*p0 + 3*mt*mt*t*p1 + 3*mt*t*t*p2 + t*t*t*p3;
}

/* ── Write PSF2 format ──────────────────────────────────────────── */

function writePSF2(glyphs, width, height, outputPath) {
    const PSF2_MAGIC = 0x864AB572;
    const HEADER_SIZE = 32;
    const bytesPerRow = Math.ceil(width / 8);
    const bytesPerGlyph = bytesPerRow * height;

    const headerBuf = Buffer.alloc(HEADER_SIZE);
    headerBuf.writeUInt32LE(PSF2_MAGIC, 0);       /* magic */
    headerBuf.writeUInt32LE(0, 4);                  /* version */
    headerBuf.writeUInt32LE(HEADER_SIZE, 8);        /* header size */
    headerBuf.writeUInt32LE(0, 12);                 /* flags (no unicode table) */
    headerBuf.writeUInt32LE(glyphs.length, 16);     /* glyph count */
    headerBuf.writeUInt32LE(bytesPerGlyph, 20);     /* bytes per glyph */
    headerBuf.writeUInt32LE(height, 24);            /* height */
    headerBuf.writeUInt32LE(width, 28);             /* width */

    const glyphsBuf = Buffer.alloc(glyphs.length * bytesPerGlyph);
    for (let i = 0; i < glyphs.length; i++) {
        const glyph = glyphs[i];
        const offset = i * bytesPerGlyph;
        for (let row = 0; row < height; row++) {
            glyphsBuf[offset + row * bytesPerRow] = glyph[row] || 0;
        }
    }

    const result = Buffer.concat([headerBuf, glyphsBuf]);
    fs.writeFileSync(outputPath, result);
}

/* ── Main ────────────────────────────────────────────────────────── */

console.log(`Loading font: ${inputPath}`);
const font = opentype.loadSync(inputPath);
console.log(`Font: ${font.names.fontFamily?.en || 'unknown'}, UPM: ${font.unitsPerEm}`);
console.log(`Metrics: ascender=${font.ascender}, descender=${font.descender}`);

/* Compute font size that fits ascender+descender into cell height.
 * Leave 1px padding at top only. */
const usableH = CELL_H - 1;  /* 15 usable pixels for 16px cell */
const lineSpan = font.ascender - font.descender;  /* e.g. 2708-(-660)=3368 */
const idealFontSize = (usableH * font.unitsPerEm) / lineSpan;

/* The baseline in output pixels: 1px top padding + ascender portion */
const ascenderPx = Math.round(font.ascender / lineSpan * usableH);
const baselinePx = 1 + ascenderPx;

console.log(`Font size: ${idealFontSize.toFixed(1)}px, baseline at row ${baselinePx}`);

/* Supersampled baseline for the rasterizer */
const SS = 4;
const ssBaseline = Math.round(baselinePx * SS);
const ssFontSize = idealFontSize * SS;

console.log(`Rasterizing ${GLYPH_COUNT} glyphs at ${CELL_W}x${CELL_H}...`);

const glyphs = [];
for (let i = 0; i < GLYPH_COUNT; i++) {
    glyphs.push(renderGlyph(font, i, CELL_W, CELL_H, ssBaseline, ssFontSize));
}

writePSF2(glyphs, CELL_W, CELL_H, outputPath);
console.log(`OK: ${outputPath} (${GLYPH_COUNT} glyphs, ${CELL_W}x${CELL_H})`);

/* Now run psf2h.py to generate the C header */
const headerOut = path.join(path.dirname(outputPath), '..', 'include', 'font_psf.h');
const { execSync } = require('child_process');
try {
    execSync(`python3 tools/psf2h.py "${outputPath}" "${headerOut}"`, { stdio: 'inherit' });
} catch (e) {
    console.error('Warning: psf2h.py failed, you may need to run it manually');
}
