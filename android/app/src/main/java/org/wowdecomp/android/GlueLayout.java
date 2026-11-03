package org.wowdecomp.android;

import android.util.Xml;

import org.xmlpull.v1.XmlPullParser;

import java.io.ByteArrayInputStream;
import java.util.*;

/** A deliberately limited, data-only GlueXML reader. Does not execute Scripts or Lua. */
final class GlueLayout {
    static final class Node {
        final String tag;
        final Map<String, String> attrs = new HashMap<>();
        final List<Node> children = new ArrayList<>();
        Node parent;

        Node(String t) {
            tag = t;
        }

        String attr(String k) {
            return attrs.getOrDefault(k, "");
        }

        Node child(String t) {
            for (Node n : children) if (n.tag.equals(t)) return n;
            return null;
        }

        float number(String k, float d) {
            try {
                float v = Float.parseFloat(attr(k));
                return Float.isFinite(v) && Math.abs(v) <= 100000 ? v : d;
            } catch (Exception e) {
                return d;
            }
        }
    }

    static final class Box {
        final float x, y, w, h;

        Box(float x, float y, float w, float h) {
            this.x = x;
            this.y = y;
            this.w = w;
            this.h = h;
        }
    }

    final Map<String, Node> named = new HashMap<>();
    private int count;

    void add(byte[] bytes) throws Exception {
        if (bytes == null || bytes.length > 1024 * 1024)
            throw new IllegalArgumentException("GlueXML ausente ou grande demais");
        XmlPullParser p = Xml.newPullParser();
        p.setInput(new ByteArrayInputStream(bytes), null);
        ArrayDeque<Node> stack = new ArrayDeque<>();
        for (int e = p.getEventType(); e != XmlPullParser.END_DOCUMENT; e = p.nextToken()) {
            if (e == XmlPullParser.DOCDECL)
                throw new IllegalArgumentException("DOCTYPE não permitido");
            if (e == XmlPullParser.START_TAG) {
                if (++count > 30000 || stack.size() > 64)
                    throw new IllegalArgumentException("Limite de GlueXML");
                Node n = new Node(p.getName());
                for (int i = 0; i < p.getAttributeCount(); i++)
                    n.attrs.put(p.getAttributeName(i), p.getAttributeValue(i));
                if (!stack.isEmpty()) {
                    n.parent = stack.peek();
                    n.parent.children.add(n);
                }
                String name = n.attr("name");
                if (!name.isEmpty()) {
                    Node ancestor = n.parent;
                    while (ancestor != null && ancestor.attr("name").isEmpty())
                        ancestor = ancestor.parent;
                    if (name.contains("$parent"))
                        name =
                                name.replace(
                                        "$parent", ancestor == null ? "" : ancestor.attr("name"));
                    n.attrs.put("name", name);
                    named.put(name, n);
                }
                stack.push(n);
            } else if (e == XmlPullParser.END_TAG) stack.pop();
        }
    }

    Node property(Node n, String tag) {
        return property(n, tag, new HashSet<>());
    }

    private Node property(Node n, String tag, Set<Node> seen) {
        if (n == null || !seen.add(n)) return null;
        Node own = n.child(tag);
        if (own != null) return own;
        for (String base : n.attr("inherits").split(",")) {
            Node r = property(named.get(base.trim()), tag, seen);
            if (r != null) return r;
        }
        return null;
    }

    String texture(String name) {
        Node n = named.get(name);
        Set<Node> seen = new HashSet<>();
        while (n != null && seen.add(n)) {
            if (!n.attr("file").isEmpty()) return n.attr("file");
            n = named.get(n.attr("inherits"));
        }
        return "";
    }

    Box box(String name, float width, float height) {
        return box(named.get(name), new Box(0, 0, width, height), new HashSet<>());
    }

    private Box box(Node n, Box viewport, Set<Node> chain) {
        if (n == null) return viewport;
        if (!chain.add(n) || chain.size() > 64)
            throw new IllegalArgumentException("Ciclo de âncoras GlueXML");
        Node parent = n.parent;
        while (parent != null
                && !Arrays.asList("Frame", "ModelFFX", "Button", "EditBox", "CheckButton")
                        .contains(parent.tag)) parent = parent.parent;
        if (!n.attr("parent").isEmpty()) parent = named.get(n.attr("parent"));
        Box ref = parent == null ? viewport : box(parent, viewport, new HashSet<>(chain));
        if ("true".equals(n.attr("setAllPoints"))) return ref;
        Node size = property(n, "Size");
        if (size != null && size.child("AbsDimension") != null) size = size.child("AbsDimension");
        float w = size == null ? 0 : size.number("x", 0),
                h = size == null ? 0 : size.number("y", 0);
        Node anchors = property(n, "Anchors");
        if (anchors == null) return new Box(ref.x, ref.y, w, h);
        float[] fx = new float[3], px = new float[3], fy = new float[3], py = new float[3];
        int nx = 0, ny = 0;
        for (Node a : anchors.children) {
            if (!a.tag.equals("Anchor")) continue;
            String point = a.attr("point"), rp = a.attr("relativePoint");
            if (rp.isEmpty()) rp = point;
            String relative = a.attr("relativeTo");
            if (relative.contains("$parent"))
                relative = relative.replace("$parent", parent == null ? "" : parent.attr("name"));
            Box b =
                    relative.isEmpty()
                            ? ref
                            : box(named.get(relative), viewport, new HashSet<>(chain));
            Node offset = a.child("Offset");
            if (offset != null && offset.child("AbsDimension") != null)
                offset = offset.child("AbsDimension");
            float ox = offset == null ? a.number("x", 0) : offset.number("x", 0),
                    oy = offset == null ? a.number("y", 0) : offset.number("y", 0);
            if (nx < 3
                    && (anchors.children.size() == 1
                            || !point.equals("TOP") && !point.equals("BOTTOM"))) {
                fx[nx] = axis(point, true);
                px[nx++] = b.x + b.w * axis(rp, true) + ox;
            }
            if (ny < 3
                    && (anchors.children.size() == 1
                            || !point.equals("LEFT") && !point.equals("RIGHT"))) {
                fy[ny] = axis(point, false);
                py[ny++] = b.y + b.h * axis(rp, false) - oy;
            }
        }
        // WoW TOP/LEFT/RIGHT anchors only constrain their named axis when combined.
        if (nx > 1) {
            for (int i = 0; i < nx; i++)
                for (int j = i + 1; j < nx; j++)
                    if (fx[i] != fx[j]) w = (px[j] - px[i]) / (fx[j] - fx[i]);
        }
        if (ny > 1) {
            for (int i = 0; i < ny; i++)
                for (int j = i + 1; j < ny; j++)
                    if (fy[i] != fy[j]) h = (py[j] - py[i]) / (fy[j] - fy[i]);
        }
        float x = nx == 0 ? ref.x : px[0] - fx[0] * w, y = ny == 0 ? ref.y : py[0] - fy[0] * h;
        return new Box(x, y, Math.max(0, w), Math.max(0, h));
    }

    private static float axis(String point, boolean horizontal) {
        if (horizontal) {
            if (point.contains("LEFT")) return 0;
            if (point.contains("RIGHT")) return 1;
        } else {
            if (point.contains("TOP")) return 0;
            if (point.contains("BOTTOM")) return 1;
        }
        return .5f;
    }
}
