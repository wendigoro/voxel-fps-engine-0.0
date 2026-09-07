package voxel.painter.filter;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Objects;

/**
 * Writes a bitcrush+upscale preview PNG. Occupancy grids are never modified;
 * this is display/export only.
 */
public final class PngExport {

    private PngExport() {}

    /**
     * Apply {@link BitcrushUpscale#apply(BufferedImage)} and write PNG to {@code outPath}.
     *
     * @return the filtered image that was written
     */
    public static BufferedImage writeCrushedPreview(BufferedImage unitPreview, Path outPath)
            throws IOException {
        return writeCrushedPreview(
                unitPreview,
                outPath,
                BitcrushUpscale.DEFAULT_SCALE,
                BitcrushUpscale.DEFAULT_BITS_PER_CHANNEL);
    }

    public static BufferedImage writeCrushedPreview(
            BufferedImage unitPreview,
            Path outPath,
            int scale,
            int bitsPerChannel) throws IOException {
        Objects.requireNonNull(unitPreview, "unitPreview");
        Objects.requireNonNull(outPath, "outPath");
        final BufferedImage crushed = BitcrushUpscale.apply(unitPreview, scale, bitsPerChannel);
        final Path parent = outPath.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        if (!ImageIO.write(crushed, "png", outPath.toFile())) {
            throw new IOException("ImageIO failed to write PNG: " + outPath);
        }
        return crushed;
    }
}
