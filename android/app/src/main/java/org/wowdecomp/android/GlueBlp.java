package org.wowdecomp.android;

/** Bounded BLP2 mip-zero decoder for the original Glue art. No Android dependency. */
final class GlueBlp {
    static final class Image {
        final int width, height;
        final int[] argb;

        Image(int w, int h) {
            width = w;
            height = h;
            argb = new int[w * h];
        }
    }

    private static int u(byte[] b, int p) {
        return b[p] & 255;
    }

    private static int le16(byte[] b, int p) {
        return u(b, p) | (u(b, p + 1) << 8);
    }

    private static long le32(byte[] b, int p) {
        return (long) le16(b, p) | ((long) le16(b, p + 2) << 16);
    }

    private static void require(boolean ok) {
        if (!ok) throw new IllegalArgumentException("BLP2 inválido ou não suportado");
    }

    static Image decode(byte[] b) {
        require(b != null && b.length >= 148 && b.length <= 8 * 1024 * 1024);
        require(b[0] == 'B' && b[1] == 'L' && b[2] == 'P' && b[3] == '2' && le32(b, 4) == 1);
        long w = le32(b, 12), h = le32(b, 16);
        require(w > 0 && h > 0 && w <= 2048 && h <= 2048 && w * h <= 2097152);
        int encoding = u(b, 8), depth = u(b, 9), alpha = u(b, 10), n = (int) (w * h);
        long off = le32(b, 20), len = le32(b, 84);
        require(off >= 148 && len > 0 && off + len <= b.length);
        int p = (int) off, end = (int) (off + len);
        Image out = new Image((int) w, (int) h);
        if (encoding == 1) {
            require(
                    b.length >= 1172
                            && off >= 1172
                            && (depth == 0 || depth == 1 || depth == 4 || depth == 8));
            require((long) p + n + ((long) n * depth + 7) / 8 <= end);
            for (int i = 0; i < n; i++) {
                int a = 255;
                if (depth == 1) a = ((u(b, p + n + i / 8) >> (i % 8)) & 1) * 255;
                if (depth == 4) a = ((u(b, p + n + i / 2) >> ((i % 2) * 4)) & 15) * 17;
                if (depth == 8) a = u(b, p + n + i);
                int c = 148 + u(b, p + i) * 4;
                out.argb[i] = (a << 24) | (u(b, c + 2) << 16) | (u(b, c + 1) << 8) | u(b, c);
            }
        } else if (encoding == 3) {
            require((long) p + n * 4L <= end);
            for (int i = 0; i < n; i++, p += 4)
                out.argb[i] =
                        (u(b, p + 3) << 24) | (u(b, p + 2) << 16) | (u(b, p + 1) << 8) | u(b, p);
        } else if (encoding == 2) {
            require(alpha == 0 || alpha == 1 || alpha == 7);
            int block = alpha == 0 ? 8 : 16, bx = (out.width + 3) / 4, by = (out.height + 3) / 4;
            require((long) p + (long) bx * by * block <= end);
            for (int y = 0; y < by; y++)
                for (int x = 0; x < bx; x++, p += block) {
                    int cp = p + (block == 8 ? 0 : 8), c0 = le16(b, cp), c1 = le16(b, cp + 2);
                    int[] c = {rgb(c0), rgb(c1), 0, 0};
                    if (c0 > c1 || block == 16) {
                        c[2] = mix(c[0], c[1], 2, 1, 3);
                        c[3] = mix(c[0], c[1], 1, 2, 3);
                    } else {
                        c[2] = mix(c[0], c[1], 1, 1, 2);
                        c[3] = depth == 0 ? 0xff000000 : 0;
                    }
                    long bits = le32(b, cp + 4), ab = 0;
                    int[] a = new int[8];
                    if (alpha == 7) {
                        a[0] = u(b, p);
                        a[1] = u(b, p + 1);
                        for (int k = 0; k < 6; k++) ab |= (long) u(b, p + 2 + k) << (k * 8);
                        if (a[0] > a[1])
                            for (int k = 2; k < 8; k++)
                                a[k] = ((8 - k) * a[0] + (k - 1) * a[1]) / 7;
                        else {
                            for (int k = 2; k < 6; k++)
                                a[k] = ((6 - k) * a[0] + (k - 1) * a[1]) / 5;
                            a[6] = 0;
                            a[7] = 255;
                        }
                    }
                    for (int i = 0; i < 16; i++) {
                        int xx = x * 4 + i % 4,
                                yy = y * 4 + i / 4,
                                col = c[(int) (bits >> (2 * i)) & 3];
                        if (alpha == 1)
                            col =
                                    (col & 0xffffff)
                                            | (((u(b, p + i / 2) >> ((i % 2) * 4)) & 15) * 17
                                                    << 24);
                        if (alpha == 7)
                            col = (col & 0xffffff) | (a[(int) (ab >> (3 * i)) & 7] << 24);
                        if (xx < out.width && yy < out.height) out.argb[yy * out.width + xx] = col;
                    }
                }
        } else throw new IllegalArgumentException("Codificação BLP2 não suportada");
        return out;
    }

    private static int rgb(int c) {
        int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
        return 0xff000000
                | ((r << 3 | r >> 2) << 16)
                | ((g << 2 | g >> 4) << 8)
                | (b << 3 | b >> 2);
    }

    private static int mix(int a, int b, int x, int y, int d) {
        return 0xff000000
                | ((((a >> 16) & 255) * x + ((b >> 16) & 255) * y) / d << 16)
                | ((((a >> 8) & 255) * x + ((b >> 8) & 255) * y) / d << 8)
                | (((a & 255) * x + (b & 255) * y) / d);
    }
}
