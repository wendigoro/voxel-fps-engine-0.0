package voxel.painter.filter;

import java.awt.image.BufferedImage;

/**
 * Display/export filter: nearest-neighbor upscale then color bitcrush.
 * <p>
 * Does <strong>not</strong> modify voxel occupancy grids. Canonical geometry remains
 * cubic unit cells ({@code VOXEL_SIZE = 0.001}, 1×1×1 integer cells). Never bake
 * crushed pixels back into painter/engine occupancy.
 */
public final class BitcrushUpscale {
    /** Default nearest-neighbor scale for painter preview/export. */
    public static final int DEFAULT_SCALE = 32;
    /** Approximate 4-bit/channel crush via 16 quantization levels. */
    public static final int DEFAULT_BITS_PER_CHANNEL = 4;

    private BitcrushUpscale() {}

    public static BufferedImage apply(BufferedImage src) {
        return apply(src, DEFAULT_SCALE, DEFAULT_BITS_PER_CHANNEL);
    }

    /**
     * @param bitsPerChannel color depth hint (2..8); mapped to quantization levels = 2^bits
     */
    public static BufferedImage apply(BufferedImage src, int scale, int bitsPerChannel) {
        if (src == null) throw new IllegalArgumentException("src");
        int w = src.getWidth();
        int h = src.getHeight();
        int[][] grid = new int[h][w];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                grid[y][x] = src.getRGB(x, y) & 0xFFFFFF;
        int bits = Math.max(2, Math.min(8, bitsPerChannel));
        int levels = 1 << bits;
        int[][] out = upscaleAndCrush(grid, scale, levels);
        BufferedImage img = new BufferedImage(out[0].length, out.length, BufferedImage.TYPE_INT_RGB);
        for (int y = 0; y < out.length; y++)
            for (int x = 0; x < out[0].length; x++)
                img.setRGB(x, y, out[y][x] | 0xFF000000);
        return img;
    }

    /**
     * @param src row-major [height][width] RGB 0xRRGGBB
     * @param scale integer upscale factor (e.g. 32)
     * @param levels quantization levels per channel (>=2)
     */
    public static int[][] upscaleAndCrush(int[][] src, int scale, int levels) {
        if (src == null || src.length == 0 || src[0].length == 0) {
            throw new IllegalArgumentException("empty src");
        }
        if (scale < 1) throw new IllegalArgumentException("scale >= 1");
        if (levels < 2) levels = 2;
        int h = src.length;
        int w = src[0].length;
        int oh = h * scale;
        int ow = w * scale;
        int[][] out = new int[oh][ow];
        for (int y = 0; y < oh; y++) {
            int sy = y / scale;
            for (int x = 0; x < ow; x++) {
                int sx = x / scale;
                out[y][x] = crushRgb(src[sy][sx], levels);
            }
        }
        return out;
    }

    public static int crushRgb(int rgb, int levels) {
        int r = (rgb >> 16) & 0xFF;
        int g = (rgb >> 8) & 0xFF;
        int b = rgb & 0xFF;
        r = quantize(r, levels);
        g = quantize(g, levels);
        b = quantize(b, levels);
        return (r << 16) | (g << 8) | b;
    }

    private static int quantize(int v, int levels) {
        float t = v / 255f;
        int q = Math.round(t * (levels - 1));
        return Math.max(0, Math.min(255, Math.round(q * (255f / (levels - 1)))));
    }
}
