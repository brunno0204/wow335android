package org.wowdecomp.android;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.charset.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

/** Read-only, bounded loader for the client's loose realmlist.wtf, never a script executor. */
final class RealmlistConfig {
    static final int MAX_BYTES = 65536;
    private static final Set<String> LOCALES =
            new HashSet<>(
                    Arrays.asList(
                            "dede", "engb", "enus", "eses", "frfr", "kokr", "zhcn", "zhtw", "encn",
                            "entw", "esmx", "ruru", "ptbr", "ptpt", "itit"));

    static final class Target {
        final File file;
        final String endpoint;

        Target(File file, String endpoint) {
            this.file = file;
            this.endpoint = endpoint;
        }
    }

    private static final Pattern ASSIGN =
            Pattern.compile("^\\s*set\\s+realmlist(?:\\s+(.*))?$", Pattern.CASE_INSENSITIVE);

    static String parse(byte[] bytes) throws IOException {
        if (bytes.length > MAX_BYTES) throw new IOException("realmlist.wtf excede 64 KiB.");
        Charset charset = StandardCharsets.UTF_8;
        int offset = 0;
        if (bytes.length >= 3
                && (bytes[0] & 255) == 239
                && (bytes[1] & 255) == 187
                && (bytes[2] & 255) == 191) offset = 3;
        else if (bytes.length >= 2 && (bytes[0] & 255) == 255 && (bytes[1] & 255) == 254) {
            charset = StandardCharsets.UTF_16LE;
            offset = 2;
        } else if (bytes.length >= 2 && (bytes[0] & 255) == 254 && (bytes[1] & 255) == 255) {
            charset = StandardCharsets.UTF_16BE;
            offset = 2;
        }
        String text;
        try {
            text =
                    charset.newDecoder()
                            .onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT)
                            .decode(ByteBuffer.wrap(bytes, offset, bytes.length - offset))
                            .toString();
        } catch (CharacterCodingException error) {
            throw new IOException("Codificação inválida no realmlist.wtf.");
        }
        if (text.indexOf('\0') >= 0) throw new IOException("realmlist.wtf contém bytes inválidos.");
        String value = null;
        for (String line : text.split("\\r\\n|\\n|\\r")) {
            Matcher m = ASSIGN.matcher(line);
            if (!m.matches()) continue;
            String tail = m.group(1);
            if (tail == null) throw new IOException("Linha set realmlist sem endereço.");
            tail = tail.trim();
            String host, remainder;
            if (tail.startsWith("\"")) {
                int end = tail.indexOf('"', 1);
                if (end < 0) throw new IOException("Aspas não fechadas no realmlist.wtf.");
                host = tail.substring(1, end);
                remainder = tail.substring(end + 1).trim();
            } else {
                int end = 0;
                while (end < tail.length() && !Character.isWhitespace(tail.charAt(end))) end++;
                host = tail.substring(0, end);
                remainder = tail.substring(end).trim();
            }
            if (!remainder.isEmpty()
                    && !remainder.startsWith("#")
                    && !remainder.startsWith("//")
                    && !remainder.startsWith(";"))
                throw new IOException("Linha set realmlist inválida.");
            value = endpoint(host);
        }
        if (value == null) throw new IOException("Não encontrei set realmlist no arquivo.");
        return value;
    }

    static String endpoint(String input) throws IOException {
        if (input.isEmpty() || input.length() > 261)
            throw new IOException("Endereço inválido no realmlist.wtf.");
        String host = input, port = "3724";
        if (input.charAt(0) == '[') {
            int end = input.indexOf(']');
            if (end < 0) throw new IOException("IPv6 inválido no realmlist.wtf.");
            host = input.substring(1, end);
            if (end + 1 < input.length()) {
                if (input.charAt(end + 1) != ':')
                    throw new IOException("Porta inválida no realmlist.wtf.");
                port = input.substring(end + 2);
            }
            if (!host.contains(":")) throw new IOException("Endereço IPv6 inválido.");
            ipv6(host);
            host = "[" + host + "]";
        } else {
            int first = input.indexOf(':'), last = input.lastIndexOf(':');
            if (first >= 0 && first != last) {
                ipv6(host);
                host = "[" + host + "]";
            } else {
                if (first >= 0) {
                    host = input.substring(0, first);
                    port = input.substring(first + 1);
                }
                if (host.isEmpty() || host.length() > 253 || !host.matches("[A-Za-z0-9.-]+"))
                    throw new IOException("Endereço inválido no realmlist.wtf.");
            }
        }
        int number;
        try {
            if (!port.matches("[0-9]{1,5}")) throw new NumberFormatException();
            number = Integer.parseInt(port);
            if (number < 1 || number > 65535) throw new NumberFormatException();
        } catch (NumberFormatException error) {
            throw new IOException("Porta deve estar entre 1 e 65535 no realmlist.wtf.");
        }
        return host + ":" + number;
    }

    // Structural check only; the existing native endpoint parser validates numeric IPv6 before DNS.
    private static void ipv6(String host) throws IOException {
        if (host.length() > 45
                || !host.matches("[0-9A-Fa-f:.]+")
                || host.indexOf(':') == host.lastIndexOf(':'))
            throw new IOException("IPv6 inválido no realmlist.wtf.");
    }

    static Target load(File data, String selected) throws IOException {
        File root = data.getCanonicalFile();
        if (!root.isDirectory()) throw new IOException("Pasta Data não encontrada.");
        File client = root.getParentFile();
        if (client == null) client = root;
        if (selected != null && !selected.trim().isEmpty()) {
            File file = new File(selected.trim());
            if (!file.isAbsolute())
                throw new IOException("Informe o caminho completo do realmlist.wtf.");
            file = file.getCanonicalFile();
            if (!file.getName().equalsIgnoreCase("realmlist.wtf") || !inside(client, file))
                throw new IOException("Escolha um realmlist.wtf dentro da pasta do cliente.");
            return read(file);
        }
        List<File> local = new ArrayList<>();
        for (File child : children(root)) {
            if (child.isDirectory()
                    && LOCALES.contains(child.getName().toLowerCase(Locale.ROOT))
                    && inside(root, child)) {
                File file = named(child, "realmlist.wtf");
                if (file != null) local.add(file);
            }
        }
        local.sort(Comparator.comparing(File::getPath));
        if (!local.isEmpty()) {
            Target target = null;
            for (File file : local) {
                Target next = read(file);
                if (target == null) target = next;
                else if (!next.endpoint.equalsIgnoreCase(target.endpoint))
                    throw new IOException(
                            "Há realmlists de idiomas com servidores diferentes. Selecione o"
                                    + " arquivo em Configuração.");
            }
            return target;
        }
        File file = named(root, "realmlist.wtf");
        if (file == null && !root.equals(client)) file = named(client, "realmlist.wtf");
        if (file == null)
            throw new IOException(
                    "realmlist.wtf não encontrado em Data/<idioma>, Data ou na pasta WoW.");
        return read(file);
    }

    private static File[] children(File folder) throws IOException {
        List<File> files = new ArrayList<>();
        try (DirectoryStream<Path> entries = Files.newDirectoryStream(folder.toPath())) {
            for (Path path : entries) {
                if (files.size() >= 256)
                    throw new IOException(
                            "Pasta contém arquivos demais para detectar o realmlist. Selecione o"
                                + " arquivo em Configuração.");
                files.add(path.toFile());
            }
        }
        return files.toArray(new File[0]);
    }

    private static File named(File folder, String name) throws IOException {
        File found = null;
        for (File file : children(folder))
            if (file.getName().equalsIgnoreCase(name)) {
                if (found != null)
                    throw new IOException("Mais de um realmlist.wtf na mesma pasta.");
                if (!inside(folder, file))
                    throw new IOException("realmlist.wtf aponta para fora da pasta do cliente.");
                found = file;
            }
        return found;
    }

    private static boolean inside(File root, File file) throws IOException {
        return file.getCanonicalFile().toPath().startsWith(root.getCanonicalFile().toPath());
    }

    private static Target read(File file) throws IOException {
        if (!file.isFile()) throw new IOException("realmlist.wtf não é um arquivo legível.");
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (InputStream stream = new FileInputStream(file)) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = stream.read(buffer)) != -1) {
                if (out.size() + count > MAX_BYTES)
                    throw new IOException("realmlist.wtf excede 64 KiB.");
                out.write(buffer, 0, count);
            }
        }
        return new Target(file.getCanonicalFile(), parse(out.toByteArray()));
    }
}
