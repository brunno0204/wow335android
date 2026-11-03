#include "GlesRenderer.h"
#include "Engine/Source/Gx/Blit.h"
#include "Engine/Source/Gx/Buffer.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>

namespace wow::android {
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void checked(const char *operation) {
    const auto error = glGetError();
    if (error != GL_NO_ERROR)
        throw std::runtime_error(std::string(operation) + ": GL error " + std::to_string(error));
}
bool powerOfTwo(uint32_t value) { return value && !(value & (value - 1)); }
GLenum bufferTarget(EGxPoolTarget target) {
    return target == GxPoolTarget_Vertex ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
}
GLenum bufferUsage(EGxPoolUsage usage) {
    return usage == GxPoolUsage_Static    ? GL_STATIC_DRAW
           : usage == GxPoolUsage_Dynamic ? GL_DYNAMIC_DRAW
                                          : GL_STREAM_DRAW;
}
GLenum textureTarget(EGxTexTarget target) {
    return target == GxTex_CubeMap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
}
GLenum faceTarget(EGxTexTarget target, uint32_t face) {
    return target == GxTex_CubeMap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
}
std::vector<uint8_t> pixels(const GlesRenderer::Image &image, uint32_t width, uint32_t height,
                            EGxTexFormat format) {
    require(uint64_t(width) * height <= 16 * 1024 * 1024,
            "GPU texture staging exceeds pixel limit");
    const bool dxt = format >= GxTex_Dxt1 && format <= GxTex_Dxt5;
    unsigned pixelBytes = 0;
    switch (format) {
    case GxTex_Abgr8888:
    case GxTex_Argb8888:
        pixelBytes = 4;
        break;
    case GxTex_Argb4444:
    case GxTex_Argb1555:
    case GxTex_Rgb565:
        pixelBytes = 2;
        break;
    case GxTex_Dxt1:
    case GxTex_Dxt3:
    case GxTex_Dxt5:
        break;
    default:
        throw std::invalid_argument("GPU color upload format not implemented");
    }
    const uint32_t rowBytes =
        dxt ? ((width + 3) / 4) * (format == GxTex_Dxt1 ? 8 : 16) : width * pixelBytes;
    const uint32_t rows = dxt ? (height + 3) / 4 : height;
    const uint32_t stride = image.stride ? image.stride : rowBytes;
    require(stride >= rowBytes && image.bytes.size() >= uint64_t(rows - 1) * stride + rowBytes,
            "Truncated GPU texture upload");
    std::vector<uint8_t> output(size_t(width) * height * 4);
    if (dxt) {
        const uint32_t paddedWidth = ((width + 3) / 4) * 4, paddedHeight = rows * 4;
        std::vector<uint32_t> blocks(size_t(rowBytes) * rows / 4),
            bgra(size_t(paddedWidth) * paddedHeight);
        for (uint32_t y = 0; y < rows; ++y)
            std::memcpy(reinterpret_cast<uint8_t *>(blocks.data()) + size_t(y) * rowBytes,
                        image.bytes.data() + size_t(y) * stride, rowBytes);
        const auto inputFormat = format == GxTex_Dxt1   ? BlitFormat_Dxt1
                                 : format == GxTex_Dxt3 ? BlitFormat_Dxt3
                                                        : BlitFormat_Dxt5;
        require(Blit(C2iVector{int32_t(paddedWidth), int32_t(paddedHeight)}, BlitAlpha_0,
                     blocks.data(), rowBytes, inputFormat, bgra.data(), paddedWidth * 4,
                     BlitFormat_Argb8888) != 0,
                "GPU DXT expansion failed");
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t color = bgra[size_t(y) * paddedWidth + x];
                auto *out = output.data() + (size_t(y) * width + x) * 4;
                out[0] = color >> 16;
                out[1] = color >> 8;
                out[2] = color;
                out[3] = color >> 24;
            }
    } else {
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const auto *in = image.bytes.data() + size_t(y) * stride + size_t(x) * pixelBytes;
                auto *out = output.data() + (size_t(y) * width + x) * 4;
                if (pixelBytes == 4) {
                    out[0] = in[format == GxTex_Argb8888 ? 2 : 0];
                    out[1] = in[1];
                    out[2] = in[format == GxTex_Argb8888 ? 0 : 2];
                    out[3] = in[3];
                } else {
                    const unsigned value = in[0] | (unsigned(in[1]) << 8);
                    if (format == GxTex_Rgb565) {
                        out[0] = ((value >> 11) & 31) * 255 / 31;
                        out[1] = ((value >> 5) & 63) * 255 / 63;
                        out[2] = (value & 31) * 255 / 31;
                        out[3] = 255;
                    } else if (format == GxTex_Argb1555) {
                        out[0] = ((value >> 10) & 31) * 255 / 31;
                        out[1] = ((value >> 5) & 31) * 255 / 31;
                        out[2] = (value & 31) * 255 / 31;
                        out[3] = value & 0x8000 ? 255 : 0;
                    } else {
                        out[0] = ((value >> 8) & 15) * 17;
                        out[1] = ((value >> 4) & 15) * 17;
                        out[2] = (value & 15) * 17;
                        out[3] = ((value >> 12) & 15) * 17;
                    }
                }
            }
    }
    return output;
}
GLuint compile(GLenum type, const char *source) {
    const GLuint shader = glCreateShader(type);
    if (!shader)
        throw std::runtime_error("Cannot allocate GLES shader");
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        glDeleteShader(shader);
        throw std::runtime_error(std::string("GLES shader compilation: ") + log);
    }
    return shader;
}
} // namespace
struct GlesRenderer::Impl {
    struct BufferData {
        EGxPoolTarget target;
        EGxPoolUsage usage;
        std::vector<uint8_t> bytes;
        GLuint name = 0;
        // Index buffers: largest index per 256-index block, so draws validate ranges in
        // O(count/256) instead of reading every index on the CPU each draw.
        std::vector<uint16_t> blockMax;
        void refreshBlocks(size_t byteOffset, size_t byteSize) {
            if (target != GxPoolTarget_Index)
                return;
            const size_t indices = bytes.size() / 2, blocks = (indices + 255) / 256;
            if (blockMax.size() != blocks)
                blockMax.assign(blocks, 0xffff), byteOffset = 0, byteSize = bytes.size();
            const size_t first = byteOffset / 2 / 256,
                         last = std::min(blocks, (byteOffset + byteSize + 511) / 2 / 256 + 1);
            for (size_t b = first; b < last; ++b) {
                uint16_t high = 0;
                const size_t end = std::min(indices, (b + 1) * 256);
                for (size_t i = b * 256; i < end; ++i) {
                    uint16_t v;
                    std::memcpy(&v, bytes.data() + i * 2, 2);
                    high = std::max(high, v);
                }
                blockMax[b] = high;
            }
        }
    };
    // Redundant GL state filtering for the per-draw fixed state.
    struct GlState {
        int blend = -1, depthTest = -1, depthFunc = -1, depthMask = -1, polygonOffset = -1,
            colorMask = -1, cull = -1, frontFace = -1, scissor = -1, lighting = -1;
        GLuint arrayBuffer = 0, elementBuffer = 0, program = 0;
        size_t attribKey = SIZE_MAX;
        float polygonOffsetValue = 0;
    } gl;
    struct Upload {
        uint32_t face, level;
        std::vector<uint8_t> rgba;
    };
    struct TextureData {
        TextureDesc description;
        std::vector<Upload> uploads;
        GLuint name = 0;
        std::array<GLuint, 6> framebuffers{}, depthbuffers{};
    };
    std::map<uint64_t, BufferData> buffers;
    std::map<uint64_t, TextureData> textures;
    uint64_t nextHandle = 1, target = 0;
    uint32_t targetFace = 0, surfaceWidth = 1, surfaceHeight = 1, width = 1, height = 1;
    EGLContext context = EGL_NO_CONTEXT;
    std::thread::id thread;
    GLint maxTexture = 0, maxCube = 0;
    bool fullNpot = false;
    GLuint program = 0, whiteTexture = 0;
    GLint transformUniform = -1, alphaUniform = -1, textureTransformUniform = -1;
    GLint fogParamsUniform = -1, fogColorUniform = -1, eyeZUniform = -1;
    GLint colorOpUniform = -1, alphaOpUniform = -1;
    GLint secondTransformUniform = -1, secondColorOpUniform = -1, secondAlphaOpUniform = -1;
    GLint coordinateUniform = -1, secondCoordinateUniform = -1;

    GLint lightingUniform = -1, normalizeUniform = -1, modelViewUniform = -1, normalUniform = -1;
    GLint specularUniform = -1, emissiveUniform = -1, shininessUniform = -1;
    struct LightUniforms {
        GLint position, ambient, diffuse, specular, attenuation;
    };
    LightUniforms lightUniforms[4]{};

    void current() const {
        if (context == EGL_NO_CONTEXT || thread != std::this_thread::get_id() ||
            eglGetCurrentContext() != context)
            throw std::logic_error("GLES operation requires its attached EGL context and thread");
    }
    template <class Map> static auto &lookup(Map &map, uint64_t handle) {
        const auto found = map.find(handle);
        require(handle && found != map.end(), "Invalid or destroyed GPU resource handle");
        return found->second;
    }
    void forgetNames() {
        for (auto &pair : buffers)
            pair.second.name = 0;
        for (auto &pair : textures) {
            pair.second.name = 0;
            pair.second.framebuffers.fill(0);
            pair.second.depthbuffers.fill(0);
        }
        program = whiteTexture = 0;
        gl = GlState{};
        transformUniform = alphaUniform = textureTransformUniform = -1;
        fogParamsUniform = fogColorUniform = eyeZUniform = -1;
        colorOpUniform = alphaOpUniform = -1;
        secondTransformUniform = secondColorOpUniform = secondAlphaOpUniform = -1;
        coordinateUniform = secondCoordinateUniform = -1;
        lightingUniform = normalizeUniform = modelViewUniform = normalUniform = -1;
        specularUniform = emissiveUniform = shininessUniform = -1;
        for (auto &light : lightUniforms)
            light = {-1, -1, -1, -1, -1};
        context = EGL_NO_CONTEXT;
        thread = {};
    }
    void deleteTextureNames(TextureData &texture) {
        glDeleteFramebuffers(6, texture.framebuffers.data());
        glDeleteRenderbuffers(6, texture.depthbuffers.data());
        if (texture.name)
            glDeleteTextures(1, &texture.name);
    }
    void deleteNames() {
        for (auto &pair : buffers)
            if (pair.second.name)
                glDeleteBuffers(1, &pair.second.name);
        for (auto &pair : textures)
            deleteTextureNames(pair.second);
        if (program)
            glDeleteProgram(program);
        if (whiteTexture)
            glDeleteTextures(1, &whiteTexture);
        forgetNames();
    }
    ~Impl() {
        if (context != EGL_NO_CONTEXT && thread == std::this_thread::get_id() &&
            eglGetCurrentContext() == context)
            deleteNames();
    }
    void buffer(BufferData &data) {
        if (data.name)
            return;
        glGenBuffers(1, &data.name);
        glBindBuffer(bufferTarget(data.target), data.name);
        if (data.target == GxPoolTarget_Vertex)
            gl.arrayBuffer = data.name, gl.attribKey = SIZE_MAX;
        else
            gl.elementBuffer = data.name;
        glBufferData(bufferTarget(data.target), data.bytes.size(), data.bytes.data(),
                     bufferUsage(data.usage));
        try {
            checked("Upload Gx buffer");
        } catch (...) {
            if (data.name)
                glDeleteBuffers(1, &data.name);
            data.name = 0;
            throw;
        }
    }
    void texture(TextureData &data) {
        if (data.name)
            return;
        const auto &d = data.description;
        require(d.width <= uint32_t(d.target == GxTex_CubeMap ? maxCube : maxTexture) &&
                    d.height <= uint32_t(d.target == GxTex_CubeMap ? maxCube : maxTexture),
                "Texture exceeds recreated context limits");
        const auto bind = textureTarget(d.target);
        glGenTextures(1, &data.name);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(bind, data.name);
        static const GLint filters[] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                        GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR};
        glTexParameteri(bind, GL_TEXTURE_MIN_FILTER, filters[d.filter]);
        glTexParameteri(bind, GL_TEXTURE_MAG_FILTER,
                        d.filter == GxTex_Nearest || d.filter == GxTex_NearestMipNearest
                            ? GL_NEAREST
                            : GL_LINEAR);
        glTexParameteri(bind, GL_TEXTURE_WRAP_S, d.wrapU ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(bind, GL_TEXTURE_WRAP_T, d.wrapV ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (const auto &upload : data.uploads)
            glTexImage2D(faceTarget(d.target, upload.face), upload.level, GL_RGBA,
                         std::max(1u, d.width >> upload.level),
                         std::max(1u, d.height >> upload.level), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         upload.rgba.data());
        if (d.generateMips)
            glGenerateMipmap(bind);
        if (std::getenv("WOWPORT_TEXDEBUG") && d.filter >= 2 && !d.generateMips) {
            uint32_t levels = 1;
            for (uint32_t w = d.width, h = d.height; w > 1 || h > 1; w = std::max(1u, w >> 1), h = std::max(1u, h >> 1))
                ++levels;
            uint32_t have = 0;
            for (const auto &upload : data.uploads)
                have = std::max(have, upload.level + 1);
            if (have < levels)
                std::fprintf(stderr, "TEXDEBUG incomplete %ux%u filter %d levels %u/%u\n", d.width, d.height, int(d.filter), have, levels);
        }
        try {
            checked("Upload Gx texture");
        } catch (...) {
            if (data.name)
                glDeleteTextures(1, &data.name);
            data.name = 0;
            throw;
        }
    }
    void bindTarget() {
        if (!target) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            width = surfaceWidth;
            height = surfaceHeight;
        } else {
            auto &data = lookup(textures, target);
            texture(data);
            const auto &d = data.description;
            auto &framebuffer = data.framebuffers[targetFace];
            auto &depth = data.depthbuffers[targetFace];
            if (!framebuffer) {
                glGenFramebuffers(1, &framebuffer);
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       faceTarget(d.target, targetFace), data.name, 0);
                glGenRenderbuffers(1, &depth);
                glBindRenderbuffer(GL_RENDERBUFFER, depth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, d.width, d.height);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                          depth);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    glDeleteFramebuffers(1, &framebuffer);
                    glDeleteRenderbuffers(1, &depth);
                    framebuffer = depth = 0;
                    throw std::runtime_error("GLES render target is incomplete");
                }
            } else
                glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
            width = d.width;
            height = d.height;
        }
        glViewport(0, 0, width, height);
        checked("Bind Gx render target");
    }
    void unlitProgram() {
        if (program)
            return;
        const char *vertex =
            "attribute vec3 a_position; attribute vec4 a_color; attribute vec3 "
            "a_normal;"
            "attribute vec2 a_texcoord; attribute vec2 a_texcoord1;"
            "uniform mat4 u_transform; uniform mat4 u_textureTransform; uniform "
            "mat4 u_textureTransform1;"
            "uniform int u_coord; uniform int u_coord1;"
            "varying vec4 v_color; varying vec3 v_uv; varying vec3 v_uv1; varying "
            "float v_fog;"
            "uniform vec4 u_eyeZ; uniform vec2 u_fogParams;"
            "uniform bool u_lighting; uniform bool u_normalize; uniform mat4 "
            "u_modelView;"
            "uniform mat3 u_normal; uniform vec3 u_specular; uniform vec3 "
            "u_emissive;"
            "uniform float u_shininess; varying vec3 v_specular;"
            "uniform vec4 u_lightPosition[4]; uniform vec3 u_lightAmbient[4];"
            "uniform vec3 u_lightDiffuse[4]; uniform vec3 u_lightSpecular[4];"
            "uniform vec4 u_lightAttenuation[4];"
            "vec3 unit(vec3 v){float d=dot(v,v);return "
            "d>0.0?v*inversesqrt(d):vec3(0.0);}"
            "void "
            "main(){gl_Position=u_transform*vec4(a_position,1.0);gl_PointSize=1.0;"
            "v_color=a_color.bgra;v_specular=vec3(0.0);"
            "if(u_lighting){vec3 eye=(u_modelView*vec4(a_position,1.0)).xyz;"
            "vec3 n=u_normal*a_normal;if(u_normalize)n=unit(n);"
            "vec3 result=u_emissive;vec3 spec=vec3(0.0);"
            "for(int i=0;i<4;++i){if(u_lightAttenuation[i].w>0.0){"
            "vec3 delta=u_lightPosition[i].xyz-eye*u_lightPosition[i].w;"
            "float d=length(delta);vec3 l=unit(delta);float attenuation=1.0;"
            "if(u_lightPosition[i].w>0.0)attenuation=1.0/"
            "max(dot(u_lightAttenuation[i].xyz,vec3(1.0,d,d*d)),0.000001);"
            "float diffuse=max(dot(n,l),0.0);"
            "result+=attenuation*v_color.rgb*(u_lightAmbient[i]+u_lightDiffuse[i]*"
            "diffuse);"
            "if(diffuse>0.0){float h=max(dot(n,unit(l+unit(-eye))),0.0);"
            "float power=u_shininess==0.0?1.0:pow(h,u_shininess);"
            "spec+=attenuation*u_specular*u_lightSpecular[i]*power;}}}"
            "v_color.rgb=clamp(result,0.0,1.0);v_specular=clamp(spec,0.0,1.0);}"

            "v_uv=(u_textureTransform*vec4(u_coord==0?a_texcoord:a_texcoord1,0.0,1."
            "0)).xyw;"
            "v_uv1=(u_textureTransform1*vec4(u_coord1==0?a_texcoord:a_texcoord1,0."
            "0,1.0)).xyw;"
            "v_fog=(u_fogParams.x-abs(dot(u_eyeZ,vec4(a_position,1.0))))*u_"
            "fogParams.y;}";
        // Match s_texEnvOps. PREVIOUS is primary color at stage 0, then the
        // clamped stage-0 result at stage 1; Decal/Fade use PREVIOUS alpha.
        const char *fragment =
            "precision mediump float; varying vec4 v_color; varying vec3 v_uv; "
            "varying vec3 v_uv1;"
            "uniform sampler2D u_texture; uniform sampler2D u_texture1; uniform "
            "float u_alphaRef;"
            "varying float v_fog; varying vec3 v_specular; uniform vec4 u_fogColor;"
            "uniform int u_colorOp; uniform int u_alphaOp; uniform int u_colorOp1; "
            "uniform int u_alphaOp1;"
            "vec4 combine(int op,vec4 p,vec4 t){"
            "if(op==0)return p*t;"
            "if(op==1)return clamp(2.0*p*t,0.0,1.0);"
            "if(op==2)return clamp(p+t,0.0,1.0);"
            "if(op==3)return p;"
            "if(op==4)return mix(t,p,p.a);"
            "return mix(p,t,p.a);}"
            "void main(){vec4 t=texture2DProj(u_texture,v_uv);"
            "vec4 "
            "c=vec4(combine(u_colorOp,v_color,t).rgb,combine(u_alphaOp,v_color,t)."
            "a);"
            "vec4 t1=texture2DProj(u_texture1,v_uv1);"
            "c=vec4(combine(u_colorOp1,c,t1).rgb,combine(u_alphaOp1,c,t1).a);"
            "if(c.a<u_alphaRef)discard;c.rgb=clamp(c.rgb+v_specular,0.0,1.0);"
            "c.rgb=mix(c.rgb,u_fogColor.rgb,(1.0-clamp(v_fog,0.0,1.0))*u_fogColor."
            "a);"
            "gl_FragColor=c;}";
        GLuint vs = 0, fs = 0, linked = 0;
        try {
            vs = compile(GL_VERTEX_SHADER, vertex);
            fs = compile(GL_FRAGMENT_SHADER, fragment);
            linked = glCreateProgram();
            if (!linked)
                throw std::runtime_error("Cannot allocate GLES program");
            glAttachShader(linked, vs);
            glAttachShader(linked, fs);
            glBindAttribLocation(linked, 0, "a_position");
            glBindAttribLocation(linked, 1, "a_color");
            glBindAttribLocation(linked, 2, "a_texcoord");
            glBindAttribLocation(linked, 3, "a_texcoord1");
            glBindAttribLocation(linked, 4, "a_normal");
            glLinkProgram(linked);
            GLint success = 0;
            glGetProgramiv(linked, GL_LINK_STATUS, &success);
            if (!success) {
                char log[2048]{};
                glGetProgramInfoLog(linked, sizeof(log), nullptr, log);
                throw std::runtime_error(std::string("GLES program link: ") + log);
            }
            glUseProgram(linked);
            gl.program = linked;
            glUniform1i(glGetUniformLocation(linked, "u_texture"), 0);
            glUniform1i(glGetUniformLocation(linked, "u_texture1"), 1);
            transformUniform = glGetUniformLocation(linked, "u_transform");
            alphaUniform = glGetUniformLocation(linked, "u_alphaRef");
            textureTransformUniform = glGetUniformLocation(linked, "u_textureTransform");
            fogParamsUniform = glGetUniformLocation(linked, "u_fogParams");
            fogColorUniform = glGetUniformLocation(linked, "u_fogColor");
            eyeZUniform = glGetUniformLocation(linked, "u_eyeZ");
            colorOpUniform = glGetUniformLocation(linked, "u_colorOp");
            alphaOpUniform = glGetUniformLocation(linked, "u_alphaOp");
            secondTransformUniform = glGetUniformLocation(linked, "u_textureTransform1");
            secondColorOpUniform = glGetUniformLocation(linked, "u_colorOp1");
            secondAlphaOpUniform = glGetUniformLocation(linked, "u_alphaOp1");
            coordinateUniform = glGetUniformLocation(linked, "u_coord");
            secondCoordinateUniform = glGetUniformLocation(linked, "u_coord1");
            lightingUniform = glGetUniformLocation(linked, "u_lighting");
            normalizeUniform = glGetUniformLocation(linked, "u_normalize");
            modelViewUniform = glGetUniformLocation(linked, "u_modelView");
            normalUniform = glGetUniformLocation(linked, "u_normal");
            specularUniform = glGetUniformLocation(linked, "u_specular");
            emissiveUniform = glGetUniformLocation(linked, "u_emissive");
            shininessUniform = glGetUniformLocation(linked, "u_shininess");
            for (unsigned i = 0; i < 4; ++i) {
                auto location = [&](const char *name) {
                    return glGetUniformLocation(
                        linked, (std::string(name) + "[" + std::to_string(i) + "]").c_str());
                };
                lightUniforms[i] = {location("u_lightPosition"), location("u_lightAmbient"),
                                    location("u_lightDiffuse"), location("u_lightSpecular"),
                                    location("u_lightAttenuation")};
            }
            checked("Create Gx fixed material program");
            program = linked;
        } catch (...) {
            if (vs)
                glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
            if (linked)
                glDeleteProgram(linked);
            throw;
        }
        glDeleteShader(vs);
        glDeleteShader(fs);
    }
};
GlesRenderer::GlesRenderer() : impl(std::make_unique<Impl>()) {}
GlesRenderer::~GlesRenderer() = default;
void GlesRenderer::attachCurrentContext() {
    require(eglGetCurrentContext() != EGL_NO_CONTEXT, "No current EGL context");
    if (impl->context != EGL_NO_CONTEXT)
        throw std::logic_error(
            "Release or lose the previous GLES context before attaching another");
    const auto *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    require(version && (std::string(version).find("OpenGL ES 2.") == 0 ||
                        std::string(version).find("OpenGL ES 3.") == 0),
            "An OpenGL ES 2 or 3 context is required");
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &impl->maxTexture);
    glGetIntegerv(GL_MAX_CUBE_MAP_TEXTURE_SIZE, &impl->maxCube);
    const auto *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
    impl->fullNpot =
        std::string(version).find("OpenGL ES 2.") == std::string::npos ||
        (extensions &&
         (std::string(" ") + extensions + " ").find(" GL_OES_texture_npot ") != std::string::npos);
    checked("Attach GLES context");
    impl->context = eglGetCurrentContext();
    impl->thread = std::this_thread::get_id();
}
void GlesRenderer::releaseCurrentContext() {
    impl->current();
    impl->deleteNames();
    checked("Release GLES resources");
}
void GlesRenderer::loseContext() { impl->forgetNames(); }
void GlesRenderer::resize(uint32_t width, uint32_t height) {
    impl->current();
    require(width && height && width <= INT32_MAX && height <= INT32_MAX,
            "Invalid GLES surface size");
    impl->surfaceWidth = width;
    impl->surfaceHeight = height;
    impl->bindTarget();
}
GlesRenderer::Buffer GlesRenderer::createBuffer(EGxPoolTarget target, EGxPoolUsage usage,
                                                const void *data, size_t size) {
    impl->current();
    require(target >= GxPoolTarget_Vertex && target < GxPoolTargets_Last &&
                usage >= GxPoolUsage_Static && usage < GxPoolUsages_Last,
            "Invalid Gx buffer type");
    require(size && size <= size_t(std::numeric_limits<GLsizeiptr>::max()),
            "Invalid Gx buffer size");
    Impl::BufferData value{target, usage, std::vector<uint8_t>(size)};
    if (data)
        std::memcpy(value.bytes.data(), data, size);
    const auto handle = impl->nextHandle++;
    auto &stored = impl->buffers.emplace(handle, std::move(value)).first->second;
    stored.refreshBlocks(0, stored.bytes.size());
    try {
        impl->buffer(stored);
    } catch (...) {
        if (stored.name)
            glDeleteBuffers(1, &stored.name);
        impl->buffers.erase(handle);
        throw;
    }
    return {handle};
}
void GlesRenderer::updateTexture(Texture handle, const Image &image, EGxTexFormat format,
                                 uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    impl->current();
    auto &value = Impl::lookup(impl->textures, handle.value);
    auto found = std::find_if(value.uploads.begin(), value.uploads.end(), [&](const auto &upload) {
        return upload.face == image.face && upload.level == image.level;
    });
    require(found != value.uploads.end(), "Cannot update a missing or generated texture mip");
    const uint32_t levelWidth = std::max(1u, value.description.width >> image.level);
    const uint32_t levelHeight = std::max(1u, value.description.height >> image.level);
    require(width && height && x <= levelWidth && width <= levelWidth - x && y <= levelHeight &&
                height <= levelHeight - y,
            "GPU texture update exceeds face/mip bounds");
    const auto rgba = pixels(image, width, height, format);
    impl->texture(value);
    const auto bind = textureTarget(value.description.target);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(bind, value.name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(faceTarget(value.description.target, image.face), image.level, x, y, width,
                    height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (value.description.generateMips)
        glGenerateMipmap(bind);
    checked("Update Gx texture region");
    for (uint32_t row = 0; row < height; ++row)
        std::memcpy(found->rgba.data() + (size_t(y + row) * levelWidth + x) * 4,
                    rgba.data() + size_t(row) * width * 4, size_t(width) * 4);
}
void GlesRenderer::updateBuffer(Buffer handle, size_t offset, const void *data, size_t size) {
    impl->current();
    auto &value = Impl::lookup(impl->buffers, handle.value);
    require(offset <= value.bytes.size() && size <= value.bytes.size() - offset && (!size || data),
            "GPU buffer update exceeds allocation");
    if (!size)
        return;
    impl->buffer(value);
    glBindBuffer(bufferTarget(value.target), value.name);
    if (value.target == GxPoolTarget_Vertex)
        impl->gl.arrayBuffer = value.name, impl->gl.attribKey = SIZE_MAX;
    else
        impl->gl.elementBuffer = value.name;
    glBufferSubData(bufferTarget(value.target), offset, size, data);
    std::memcpy(value.bytes.data() + offset, data, size);
    value.refreshBlocks(offset, size);
}
void GlesRenderer::destroyBuffer(Buffer handle) {
    impl->current();
    auto &value = Impl::lookup(impl->buffers, handle.value);
    if (value.name == impl->gl.arrayBuffer)
        impl->gl.arrayBuffer = 0, impl->gl.attribKey = SIZE_MAX;
    if (value.name == impl->gl.elementBuffer)
        impl->gl.elementBuffer = 0;
    if (value.name)
        glDeleteBuffers(1, &value.name);
    impl->buffers.erase(handle.value);
}
GlesRenderer::Texture GlesRenderer::createTexture(const TextureDesc &description) {
    impl->current();
    const auto &d = description;
    require(d.target == GxTex_2d || d.target == GxTex_NonPow2 || d.target == GxTex_CubeMap,
            "Unsupported GLES texture target");
    const uint32_t maximum = d.target == GxTex_CubeMap ? impl->maxCube : impl->maxTexture;
    require(d.width && d.height && d.width <= maximum && d.height <= maximum &&
                uint64_t(d.width) * d.height <= 16 * 1024 * 1024,
            "Invalid GLES texture size");
    require(d.target != GxTex_CubeMap || d.width == d.height, "Cubemap faces must be square");
    require(d.format > GxTex_Unknown && d.format <= GxTex_Dxt5,
            "Unsupported GLES color texture format");
    require(d.filter >= GxTex_Nearest && d.filter <= GxTex_LinearMipLinear,
            "Unsupported GLES texture filter");
    require(impl->fullNpot || (powerOfTwo(d.width) && powerOfTwo(d.height)) ||
                (!d.wrapU && !d.wrapV && !d.generateMips && d.filter <= GxTex_Linear),
            "ES2 NPOT texture requires clamp and non-mip filtering");
    require(!d.renderTarget || (!d.generateMips && d.filter <= GxTex_Linear),
            "Mipmapped render targets need explicit regeneration support");
    const uint32_t faces = d.target == GxTex_CubeMap ? 6 : 1;
    uint32_t levels = 1;
    for (uint32_t extent = std::max(d.width, d.height); extent > 1; extent >>= 1)
        ++levels;
    Impl::TextureData value;
    value.description = d;
    value.description.images.clear();
    std::vector<bool> present(faces * levels, false);
    for (const auto &image : d.images) {
        require(image.face < faces && image.level < levels && (!d.generateMips || image.level == 0),
                "Invalid GPU texture face/mip");
        const uint32_t slot = image.face * levels + image.level;
        require(!present[slot], "Duplicate GPU texture face/mip");
        present[slot] = true;
        value.uploads.push_back({image.face, image.level,
                                 pixels(image, std::max(1u, d.width >> image.level),
                                        std::max(1u, d.height >> image.level), d.format)});
    }
    if (d.images.empty())
        for (uint32_t face = 0; face < faces; ++face) {
            present[face * levels] = true;
            value.uploads.push_back(
                {face, 0, std::vector<uint8_t>(size_t(d.width) * d.height * 4)});
        }
    for (uint32_t face = 0; face < faces; ++face)
        require(present[face * levels], "Missing base texture face");
    if (d.filter > GxTex_Linear && !d.generateMips)
        require(std::all_of(present.begin(), present.end(), [](bool p) { return p; }),
                "Mip filter requires a complete mip chain");
    const auto handle = impl->nextHandle++;
    auto &stored = impl->textures.emplace(handle, std::move(value)).first->second;
    try {
        impl->texture(stored);
    } catch (...) {
        impl->deleteTextureNames(stored);
        impl->textures.erase(handle);
        throw;
    }
    return {handle};
}
void GlesRenderer::setTextureSampler(Texture handle, EGxTexFilter filter, bool wrapU, bool wrapV) {
    impl->current();
    auto &value = Impl::lookup(impl->textures, handle.value);
    const auto &d = value.description;
    require(filter >= GxTex_Nearest && filter <= GxTex_LinearMipLinear,
            "Unsupported GLES texture filter");
    require(!d.renderTarget || filter <= GxTex_Linear,
            "Mipmapped render targets need regeneration support");
    require(impl->fullNpot || (powerOfTwo(d.width) && powerOfTwo(d.height)) ||
                (!wrapU && !wrapV && filter <= GxTex_Linear),
            "Incomplete ES2 NPOT sampler");
    if (filter > GxTex_Linear && !d.generateMips) {
        uint32_t levels = 1;
        for (uint32_t n = std::max(d.width, d.height); n > 1; n >>= 1)
            ++levels;
        const uint32_t faces = d.target == GxTex_CubeMap ? 6 : 1;
        for (uint32_t face = 0; face < faces; ++face)
            for (uint32_t level = 0; level < levels; ++level)
                require(
                    std::any_of(value.uploads.begin(), value.uploads.end(),
                                [&](const auto &u) { return u.face == face && u.level == level; }),
                    "Sampler needs a complete mip chain");
    }
    impl->texture(value);
    const auto bind = textureTarget(d.target);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(bind, value.name);
    static const GLint filters[] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST,
                                    GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR};
    glTexParameteri(bind, GL_TEXTURE_MIN_FILTER, filters[filter]);
    glTexParameteri(bind, GL_TEXTURE_MAG_FILTER,
                    filter == GxTex_Nearest || filter == GxTex_NearestMipNearest ? GL_NEAREST
                                                                                 : GL_LINEAR);
    glTexParameteri(bind, GL_TEXTURE_WRAP_S, wrapU ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(bind, GL_TEXTURE_WRAP_T, wrapV ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    checked("Set Gx texture sampler");
    value.description.filter = filter;
    value.description.wrapU = wrapU;
    value.description.wrapV = wrapV;
}
void GlesRenderer::destroyTexture(Texture handle) {
    impl->current();
    auto &value = Impl::lookup(impl->textures, handle.value);
    if (impl->target == handle.value) {
        impl->target = 0;
        impl->targetFace = 0;
        impl->bindTarget();
    }
    impl->deleteTextureNames(value);
    impl->textures.erase(handle.value);
    checked("Destroy Gx texture");
}
void GlesRenderer::setRenderTarget(Texture handle, uint32_t face) {
    impl->current();
    if (handle.value) {
        const auto &value = Impl::lookup(impl->textures, handle.value);
        require(value.description.renderTarget &&
                    face < (value.description.target == GxTex_CubeMap ? 6u : 1u),
                "Invalid GLES render target or face");
    } else
        require(!face, "Default framebuffer has no cube faces");
    const auto old = impl->target;
    const auto oldFace = impl->targetFace;
    impl->target = handle.value;
    impl->targetFace = face;
    try {
        impl->bindTarget();
    } catch (...) {
        impl->target = old;
        impl->targetFace = oldFace;
        throw;
    }
}
void GlesRenderer::clear(CImVector color, bool depth) {
    clear(GxBufferBit_Color | (depth ? GxBufferBit_Depth : 0), color);
}
void GlesRenderer::clear(uint32_t mask, CImVector color) {
    impl->current();
    require(!(mask & ~(GxBufferBit_Color | GxBufferBit_Depth)), "Invalid Gx clear mask");
    impl->bindTarget();
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    impl->gl.scissor = 0;
    impl->gl.colorMask = 15;
    impl->gl.depthMask = 1;
    glClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    glClearDepthf(1.0f);
    glClear(((mask & GxBufferBit_Color) ? GL_COLOR_BUFFER_BIT : 0) |
            ((mask & GxBufferBit_Depth) ? GL_DEPTH_BUFFER_BIT : 0));
    checked("Clear Gx target");
}
void GlesRenderer::drawUnlit(Buffer vertices, Buffer indices, EGxVertexBufferFormat format,
                             EGxPrim primitive, uint32_t first, uint32_t count, Texture texture,
                             const C44Matrix &transform, const DrawState &state,
                             uint32_t baseVertex, size_t vertexByteOffset,
                             uint32_t vertexCountLimit) {
    impl->current();
    require(format >= GxVBF_P && format < GxVertexBufferFormats_Last && format != GxVBF_PBNT2,
            "Unlit draw cannot supply skinned vertices");
    require(primitive >= GxPrim_Points && primitive < GxPrims_Last && count && count <= INT32_MAX,
            "Invalid Gx primitive/count");
    require((primitive != GxPrim_Triangles || count % 3 == 0) &&
                (primitive != GxPrim_Lines || count % 2 == 0) &&
                ((primitive != GxPrim_TriangleStrip && primitive != GxPrim_TriangleFan) ||
                 count >= 3) &&
                (primitive != GxPrim_LineStrip || count >= 2),
            "Incomplete Gx primitive");
    require(state.blend >= GxBlend_Opaque && state.blend < GxBlends_Last &&
                std::isfinite(state.blendFactor),
            "Invalid Gx blend state");
    require(state.colorOp >= GxTexOp_Mod && state.colorOp < GxTexOps_Last &&
                state.alphaOp >= GxTexOp_Mod && state.alphaOp < GxTexOps_Last,
            "Invalid Gx texture stage operation");
    require(state.secondColorOp >= GxTexOp_Mod && state.secondColorOp < GxTexOps_Last &&
                state.secondAlphaOp >= GxTexOp_Mod && state.secondAlphaOp < GxTexOps_Last,
            "Invalid second Gx texture stage operation");
    require(state.textureCoord >= 0 && state.textureCoord <= 1 && state.secondTextureCoord >= 0 &&
                state.secondTextureCoord <= 1,
            "Invalid Gx texture coordinate set");
    require(!state.scissor ||
                (state.x >= 0 && state.y >= 0 && state.width >= 0 && state.height >= 0),
            "Invalid GLES scissor rectangle");
    require(state.depthFunc >= GxCmp_LEqual && state.depthFunc < GxCmps_Last &&
                state.colorWriteMask <= 15 && std::isfinite(state.alphaRef),
            "Invalid depth/color/alpha state");
    require(!state.viewport ||
                (state.viewportWidth >= 0 && state.viewportHeight >= 0 &&
                 std::isfinite(state.minDepth) && std::isfinite(state.maxDepth) &&
                 state.minDepth >= 0 && state.maxDepth <= 1 && state.minDepth <= state.maxDepth),
            "Invalid Gx viewport");
    require(std::isfinite(state.polygonOffset) && std::isfinite(state.polygonOffset * -65536.0f),
            "Invalid polygon depth offset");
    if (state.fog) {
        require(std::isfinite(state.fogStart) && std::isfinite(state.fogEnd) &&
                    std::isfinite(state.fogEnd - state.fogStart) &&
                    state.fogStart != state.fogEnd &&
                    std::isfinite(1.0f / (state.fogEnd - state.fogStart)),
                "Invalid linear fog interval");
        for (float component : state.fogDepthTransform)
            require(std::isfinite(component), "Invalid eye-space fog transform");
    }
    if (state.lighting) {
        require(std::isfinite(state.shininess) && state.shininess >= 0 && state.shininess <= 128,
                "Invalid material shininess");
        const float matrix[16]{
            state.modelView.a0, state.modelView.a1, state.modelView.a2, state.modelView.a3,
            state.modelView.b0, state.modelView.b1, state.modelView.b2, state.modelView.b3,
            state.modelView.c0, state.modelView.c1, state.modelView.c2, state.modelView.c3,
            state.modelView.d0, state.modelView.d1, state.modelView.d2, state.modelView.d3};
        for (float value : matrix)
            require(std::isfinite(value), "Invalid lighting model-view matrix");
        for (float value : state.normalTransform)
            require(std::isfinite(value), "Invalid normal matrix");
        for (const auto &light : state.lights)
            if (light.enabled) {
                for (float v : light.position)
                    require(std::isfinite(v), "Invalid light position");
                require(light.position[3] == 0 || light.position[3] == 1, "Invalid light kind");
                for (const auto *color : {light.ambient, light.diffuse, light.specular})
                    for (unsigned i = 0; i < 3; ++i)
                        require(std::isfinite(color[i]), "Invalid light color");
                for (float v : light.attenuation)
                    require(std::isfinite(v) && v >= 0, "Invalid light attenuation");
                require(light.position[3] == 0 || light.attenuation[0] > 0 ||
                            light.attenuation[1] > 0 || light.attenuation[2] > 0,
                        "Point light needs nonzero attenuation");
            }
    }
    const auto &layout = ::Buffer::s_vertexBufDesc[format];
    auto &vb = Impl::lookup(impl->buffers, vertices.value);
    auto &ib = Impl::lookup(impl->buffers, indices.value);
    require(vb.target == GxPoolTarget_Vertex && ib.target == GxPoolTarget_Index &&
                vertexByteOffset <= vb.bytes.size() && ib.bytes.size() % 2 == 0,
            "Incompatible Gx vertex/index buffers");
    require(first <= ib.bytes.size() / 2 && count <= ib.bytes.size() / 2 - first,
            "Index draw exceeds GPU buffer");
    const size_t availableVertices = (vb.bytes.size() - vertexByteOffset) / layout.size;
    require(!vertexCountLimit || vertexCountLimit <= availableVertices,
            "Gx vertex view exceeds GPU allocation");
    const size_t vertexCount = vertexCountLimit ? vertexCountLimit : availableVertices;
    {
        // Whole blocks by their cached maximum; partial edge blocks index by index.
        const size_t end = size_t(first) + count;
        size_t i = first;
        while (i < end) {
            const size_t block = i / 256;
            const size_t blockEnd = std::min(end, (block + 1) * 256);
            if (i % 256 == 0 && blockEnd == (block + 1) * 256 && block < ib.blockMax.size() &&
                uint64_t(baseVertex) + ib.blockMax[block] < vertexCount) {
                i = blockEnd;
                continue;
            }
            for (; i < blockEnd; ++i) {
                uint16_t index;
                std::memcpy(&index, ib.bytes.data() + i * 2, 2);
                require(uint64_t(baseVertex) + index < vertexCount,
                        "Index references an unavailable GPU vertex");
            }
        }
    }
    const Texture handles[2]{texture, state.secondTexture};
    Impl::TextureData *sampled[2]{};
    for (unsigned stage = 0; stage < 2; ++stage) {
        sampled[stage] =
            handles[stage].value ? &Impl::lookup(impl->textures, handles[stage].value) : nullptr;
        require(!sampled[stage] || sampled[stage]->description.target != GxTex_CubeMap,
                "Unlit 2D shader cannot sample a cubemap");
        require(!handles[stage].value || handles[stage].value != impl->target,
                "Render target feedback is not permitted");
    }
    impl->bindTarget();
    impl->buffer(vb);
    impl->buffer(ib);
    impl->unlitProgram();
    for (unsigned stage = 0; stage < 2; ++stage) {
        glActiveTexture(GL_TEXTURE0 + stage);
        if (sampled[stage]) {
            impl->texture(*sampled[stage]);
            glBindTexture(GL_TEXTURE_2D, sampled[stage]->name);
        } else {
            if (!impl->whiteTexture) {
                const uint8_t white[4] = {255, 255, 255, 255};
                glGenTextures(1, &impl->whiteTexture);
                glBindTexture(GL_TEXTURE_2D, impl->whiteTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            } else
                glBindTexture(GL_TEXTURE_2D, impl->whiteTexture);
        }
    }
    glActiveTexture(GL_TEXTURE0);
    static const GLenum sourceBlend[] = {GL_ONE,
                                         GL_ONE,
                                         GL_SRC_ALPHA,
                                         GL_SRC_ALPHA,
                                         GL_DST_COLOR,
                                         GL_DST_COLOR,
                                         GL_DST_COLOR,
                                         GL_ONE_MINUS_SRC_ALPHA,
                                         GL_ONE_MINUS_SRC_ALPHA,
                                         GL_SRC_ALPHA,
                                         GL_ONE,
                                         GL_CONSTANT_ALPHA};
    static const GLenum destBlend[] = {GL_ZERO, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA,
                                       GL_ONE,  GL_ZERO, GL_SRC_COLOR,
                                       GL_ONE,  GL_ONE,  GL_ZERO,
                                       GL_ZERO, GL_ONE,  GL_ONE_MINUS_CONSTANT_ALPHA};
    static const uint8_t alphaRef[] = {0, 224, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
    auto &gl = impl->gl;
    const int blendKey = state.blend == GxBlend_Opaque || state.blend == GxBlend_AlphaKey ? 0 : int(state.blend) + 1;
    if (blendKey != gl.blend || blendKey == GxBlend_ConstantAlpha + 1) {
        if (!blendKey)
            glDisable(GL_BLEND);
        else {
            if (gl.blend <= 0)
                glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(sourceBlend[state.blend], destBlend[state.blend]);
            glBlendColor(1, 1, 1, std::clamp(state.blendFactor, 0.0f, 1.0f));
        }
        gl.blend = blendKey;
    }
    if (int(state.depthTest) != gl.depthTest) {
        if (state.depthTest)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        gl.depthTest = state.depthTest;
    }
    static const GLenum depthFunctions[] = {GL_LEQUAL, GL_EQUAL, GL_GEQUAL, GL_LESS};
    if (int(state.depthFunc) != gl.depthFunc) {
        glDepthFunc(depthFunctions[state.depthFunc]);
        gl.depthFunc = state.depthFunc;
    }
    if (int(state.depthWrite) != gl.depthMask) {
        glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
        gl.depthMask = state.depthWrite;
    }
    if (int(state.polygonOffset != 0.0f) != gl.polygonOffset || state.polygonOffset != gl.polygonOffsetValue) {
        if (state.polygonOffset != 0.0f) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            // Match the original desktop OpenGL Gx backend's slope and depth units.
            glPolygonOffset(-4.0f, state.polygonOffset * -65536.0f);
        } else
            glDisable(GL_POLYGON_OFFSET_FILL);
        gl.polygonOffset = state.polygonOffset != 0.0f;
        gl.polygonOffsetValue = state.polygonOffset;
    }
    if (int(state.colorWriteMask) != gl.colorMask) {
        glColorMask(state.colorWriteMask & 1, (state.colorWriteMask >> 1) & 1,
                    (state.colorWriteMask >> 2) & 1, (state.colorWriteMask >> 3) & 1);
        gl.colorMask = int(state.colorWriteMask);
    }
    if (int(state.cullBackFaces) != gl.cull) {
        if (state.cullBackFaces) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        } else
            glDisable(GL_CULL_FACE);
        gl.cull = state.cullBackFaces;
    }
    if (state.cullBackFaces && int(state.cullClockwise) != gl.frontFace) {
        glFrontFace(state.cullClockwise ? GL_CW : GL_CCW);
        gl.frontFace = state.cullClockwise;
    }
    if (state.scissor) {
        if (gl.scissor != 1)
            glEnable(GL_SCISSOR_TEST);
        glScissor(state.x, state.y, state.width, state.height);
        gl.scissor = 1;
    } else if (gl.scissor != 0) {
        glDisable(GL_SCISSOR_TEST);
        gl.scissor = 0;
    }
    if (state.viewport)
        glViewport(state.viewportX, state.viewportY, state.viewportWidth, state.viewportHeight);
    glDepthRangef(state.minDepth, state.maxDepth);
    if (gl.program != impl->program) {
        glUseProgram(impl->program);
        gl.program = impl->program;
    }
    glUniformMatrix4fv(impl->transformUniform, 1, GL_FALSE, &transform.a0);
    if (int(state.lighting) != gl.lighting) {
        glUniform1i(impl->lightingUniform, state.lighting);
        gl.lighting = state.lighting;
    }
    // Lighting inputs are only read by the shader when lighting is enabled.
    if (state.lighting) {
    glUniform1i(impl->normalizeUniform, state.normalizeNormals);
    glUniformMatrix4fv(impl->modelViewUniform, 1, GL_FALSE, &state.modelView.a0);
    glUniformMatrix3fv(impl->normalUniform, 1, GL_FALSE, state.normalTransform);
    glUniform3f(impl->specularUniform, state.specularColor.r / 255.0f,
                state.specularColor.g / 255.0f, state.specularColor.b / 255.0f);
    glUniform3f(impl->emissiveUniform, state.emissiveColor.r / 255.0f,
                state.emissiveColor.g / 255.0f, state.emissiveColor.b / 255.0f);
    glUniform1f(impl->shininessUniform, state.lighting ? state.shininess : 0);
    for (unsigned i = 0; i < 4; ++i) {
        const auto &light = state.lights[i];
        const auto &u = impl->lightUniforms[i];
        glUniform4fv(u.position, 1, light.position);
        glUniform3fv(u.ambient, 1, light.ambient);
        glUniform3fv(u.diffuse, 1, light.diffuse);
        glUniform3fv(u.specular, 1, light.specular);
        glUniform4f(u.attenuation, light.attenuation[0], light.attenuation[1], light.attenuation[2],
                    state.lighting && light.enabled ? 1.0f : 0.0f);
    }
    }
    // With no enabled texture, the original fixed pipeline bypasses the stage.
    glUniform1i(impl->colorOpUniform, sampled[0] ? state.colorOp : GxTexOp_PassThru);
    glUniform1i(impl->alphaOpUniform, sampled[0] ? state.alphaOp : GxTexOp_PassThru);
    glUniform1i(impl->secondColorOpUniform, sampled[1] ? state.secondColorOp : GxTexOp_PassThru);
    glUniform1i(impl->secondAlphaOpUniform, sampled[1] ? state.secondAlphaOp : GxTexOp_PassThru);
    glUniform1i(impl->coordinateUniform, state.textureCoord);
    glUniform1i(impl->secondCoordinateUniform, state.secondTextureCoord);
    glUniformMatrix4fv(impl->secondTransformUniform, 1, GL_FALSE, &state.secondTextureTransform.a0);
    glUniform2f(impl->fogParamsUniform, state.fog ? state.fogEnd : 0.0f,
                state.fog ? 1.0f / (state.fogEnd - state.fogStart) : 0.0f);
    const float noFogDepth[4]{};
    glUniform4fv(impl->eyeZUniform, 1, state.fog ? state.fogDepthTransform : noFogDepth);
    glUniform4f(impl->fogColorUniform, state.fogColor.r / 255.0f, state.fogColor.g / 255.0f,
                state.fogColor.b / 255.0f, state.fog ? 1.0f : 0.0f);
    glUniform1f(impl->alphaUniform, state.alphaRef < 0 ? alphaRef[state.blend] / 255.0f
                                                       : std::clamp(state.alphaRef, 0.0f, 1.0f));
    if (gl.arrayBuffer != vb.name) {
        glBindBuffer(GL_ARRAY_BUFFER, vb.name);
        gl.arrayBuffer = vb.name;
        gl.attribKey = SIZE_MAX;
    }
    if (gl.elementBuffer != ib.name) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib.name);
        gl.elementBuffer = ib.name;
    }
    glUniformMatrix4fv(impl->textureTransformUniform, 1, GL_FALSE, &state.textureTransform.a0);
    glVertexAttrib4f(1, state.constantColor.b / 255.0f, state.constantColor.g / 255.0f,
                     state.constantColor.r / 255.0f, state.constantColor.a / 255.0f);
    // Attribute layout is rebound only when buffer, format or offset change.
    const size_t attribKey = (size_t(vb.name) << 40) ^ (size_t(format) << 32) ^
                             (vertexByteOffset + size_t(baseVertex) * layout.size);
    if (attribKey != gl.attribKey) {
    gl.attribKey = attribKey;
    for (GLuint slot = 0; slot < 5; ++slot)
        glDisableVertexAttribArray(slot);
    glVertexAttrib2f(2, 0, 0);
    glVertexAttrib2f(3, 0, 0);
    glVertexAttrib3f(4, 0, 0, 1);
    for (uint32_t i = 0; i < layout.attribCount; ++i) {
        const auto &a = layout.attribs[i];
        GLint slot = a.m_attrib == GxVA_Position    ? 0
                     : a.m_attrib == GxVA_Color0    ? 1
                     : a.m_attrib == GxVA_TexCoord0 ? 2
                     : a.m_attrib == GxVA_TexCoord1 ? 3
                     : a.m_attrib == GxVA_Normal    ? 4
                                                    : -1;
        if (slot < 0)
            continue;
        glVertexAttribPointer(
            slot,
            slot == 0 || slot == 4 ? 3
            : slot == 1            ? 4
                                   : 2,
            slot == 1 ? GL_UNSIGNED_BYTE : GL_FLOAT, slot == 1 ? GL_TRUE : GL_FALSE, layout.size,
            reinterpret_cast<void *>(vertexByteOffset + size_t(baseVertex) * layout.size +
                                     a.m_offset));
        glEnableVertexAttribArray(slot);
    }
    }
    static const GLenum primitives[] = {GL_POINTS,    GL_LINES,          GL_LINE_STRIP,
                                        GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    glDrawElements(primitives[primitive], count, GL_UNSIGNED_SHORT,
                   reinterpret_cast<void *>(size_t(first) * 2));
}
std::vector<uint8_t> GlesRenderer::readPixels(uint32_t x, uint32_t y, uint32_t width,
                                              uint32_t height) {
    impl->current();
    impl->bindTarget();
    require(width && height && x <= impl->width && width <= impl->width - x && y <= impl->height &&
                height <= impl->height - y,
            "Readback exceeds GLES target");
    require(uint64_t(width) * height <= 16 * 1024 * 1024, "GLES readback exceeds pixel limit");
    std::vector<uint8_t> result(size_t(width) * height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    checked("Read Gx target pixels");
    return result;
}
} // namespace wow::android
