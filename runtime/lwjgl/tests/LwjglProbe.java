package mineswitch.probe;

import java.nio.ByteBuffer;
import java.nio.IntBuffer;
import java.util.Base64;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.lwjgl.glfw.GLFWCursorPosCallback;
import org.lwjgl.glfw.GLFWErrorCallback;
import org.lwjgl.glfw.GLFWWindowSizeCallback;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GLCapabilities;
import org.lwjgl.stb.STBImage;
import org.lwjgl.system.JNI;
import org.lwjgl.system.Callback;
import org.lwjgl.system.HorizonLibrary;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.system.MemoryUtil;
import org.lwjgl.system.Platform;
import static org.lwjgl.glfw.GLFW.*;
import static org.lwjgl.opengl.GL11C.*;
import static org.lwjgl.opengl.GL15C.*;
import static org.lwjgl.opengl.GL20C.*;
import static org.lwjgl.opengl.GL30C.*;

/** Uses actual upstream LWJGL bindings, real libffi closures and the Switch GPU. */
public final class LwjglProbe {
    private static int checks;
    private static long GLFW_LIBRARY_QUERY() {
        // The Minecraft dependency set has no LWJGL Vulkan module. Query the
        // actual stable GLFW capability function without initializing 3.4's
        // optional Vulkan binding class or claiming a Vulkan driver exists.
        return org.lwjgl.glfw.GLFW.getLibrary().getFunctionAddress("glfwVulkanSupported");
    }
    private static void check(String name, boolean success) {
        if (!success) throw new AssertionError(name);
        ++checks;
        System.out.println("lwjgl.check " + name + " PASS");
    }
    private static Callback stressCallback(int index,int payload,long window,boolean[] seen) {
        if ((index&1)==0) return GLFWCursorPosCallback.create((handle,x,y) ->
            seen[index]=handle==window && x==payload+0.25 && y==-payload-0.5);
        return GLFWWindowSizeCallback.create((handle,width,height) ->
            seen[index]=handle==window && width==payload+7 && height==payload+11);
    }
    private static void invokeStress(Callback callback,int index,int payload,long window) {
        if ((index&1)==0) JNI.invokePV(window,payload+0.25,-payload-0.5,callback.address());
        else JNI.invokePV(window,payload+7,payload+11,callback.address());
    }
    private static boolean allSeen(boolean[] values,boolean oddOnly) {
        for (int i=0;i<values.length;i++) if (values[i]!=( !oddOnly || (i&1)!=0)) return false;
        return true;
    }
    private static void stressClosures(long window) {
        long[] baseline=HorizonLibrary.closureStats();
        Callback[] callbacks=new Callback[128];
        boolean[] seen=new boolean[callbacks.length];
        try {
            for (int i=0;i<callbacks.length;i++) callbacks[i]=stressCallback(i,i,window,seen);
            long[] full=HorizonLibrary.closureStats();
            System.out.println("FFI_SLABS live="+full[0]+" pools="+full[1]);
            check("ffi_128_live",full[0]==baseline[0]+callbacks.length);
            check("ffi_shared_slab",full[1]<=baseline[1]+1);
            for (int i=0;i<callbacks.length;i++) invokeStress(callbacks[i],i,i,window);
            check("ffi_128_invocations",allSeen(seen,false));
            for (int i=0;i<callbacks.length;i+=2) { callbacks[i].free(); callbacks[i]=null; }
            Arrays.fill(seen,false);
            for (int i=1;i<callbacks.length;i+=2) invokeStress(callbacks[i],i,i,window);
            check("ffi_live_peers_after_sparse_free",allSeen(seen,true));
            for (int i=0;i<callbacks.length;i+=2) callbacks[i]=stressCallback(i,i+1000,window,seen);
            long[] reused=HorizonLibrary.closureStats();
            check("ffi_slot_reuse",reused[0]==full[0] && reused[1]==full[1]);
            Arrays.fill(seen,false);
            for (int i=0;i<callbacks.length;i++) invokeStress(callbacks[i],i,(i&1)==0?i+1000:i,window);
            check("ffi_reused_invocations",allSeen(seen,false));
        } finally {
            for (Callback callback:callbacks) if (callback!=null) callback.free();
        }
        long[] after=HorizonLibrary.closureStats();
        check("ffi_free_to_baseline",after[0]==baseline[0] && after[1]==baseline[1]);
    }
    private static int shader(int kind,String source) {
        int shader=glCreateShader(kind);
        glShaderSource(shader,source);
        glCompileShader(shader);
        if (glGetShaderi(shader,GL_COMPILE_STATUS)==GL_FALSE)
            throw new AssertionError("Shader compilation: " + glGetShaderInfoLog(shader));
        return shader;
    }
    private static void pixel(int x,int y,int r,int g,int b) {
        try (MemoryStack stack=MemoryStack.stackPush()) {
            ByteBuffer color=stack.malloc(4);
            glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,color);
            int red=color.get(0)&255,green=color.get(1)&255,blue=color.get(2)&255;
            System.out.println("lwjgl.pixel " + x + "," + y + " = " + red + "," + green + "," + blue);
            check("gpu_pixel_" + x + "_" + y,Math.abs(red-r)<=2 && Math.abs(green-g)<=2 && Math.abs(blue-b)<=2);
        }
    }
    private static void imageDecode() {
        // Generated independently as a one-pixel RGBA PNG with ordinary zlib/CRC.
        byte[] png=Base64.getDecoder().decode(PNG);
        ByteBuffer encoded=MemoryUtil.memAlloc(png.length);
        encoded.put(png).flip();
        try (MemoryStack stack=MemoryStack.stackPush()) {
            IntBuffer width=stack.mallocInt(1),height=stack.mallocInt(1),channels=stack.mallocInt(1);
            ByteBuffer pixels=STBImage.stbi_load_from_memory(encoded,width,height,channels,4);
            if (pixels==null) throw new AssertionError("STB PNG decode: " + STBImage.stbi_failure_reason());
            try {
                check("stb_png_dimensions",width.get(0)==1 && height.get(0)==1);
                check("stb_png_pixel",(pixels.get(0)&255)==255 && pixels.get(1)==0 && pixels.get(2)==0 && (pixels.get(3)&255)==255);
            } finally { STBImage.stbi_image_free(pixels); }
        } finally { MemoryUtil.memFree(encoded); }
    }
    private static final String PNG="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
    public static void main(String[] arguments) {
        System.out.println("MINESWITCH_LWJGL_BEGIN version=" + org.lwjgl.Version.getVersion());
        check("horizon_platform",Platform.get()==Platform.HORIZON);
        check("arm64_architecture",Platform.getArchitecture()==Platform.Architecture.ARM64);
        AtomicInteger errors=new AtomicInteger();
        GLFWErrorCallback errorCallback=GLFWErrorCallback.create((code,description) -> {
            errors.incrementAndGet();
            System.out.println("GLFW_CALLBACK code=" + code + " text=" + GLFWErrorCallback.getDescription(description));
        });
        long window=0;
        boolean initialized=false,callbackInstalled=false,glReady=false;
        int program=0,vertices=0,vao=0,vertex=0,fragment=0;
        try {
            errorCallback.set();
            callbackInstalled=true;
            int before=errors.get();
            glfwInitHint(0x7fffffff,0); // Documented invalid-enum API error, exercises the actual native callback.
            check("ffi_error_callback",errors.get()==before+1);
            check("glfw_error_code",glfwGetError(null)==GLFW_INVALID_ENUM);
            check("glfw_init",glfwInit());
            initialized=true;
            long vulkanQuery=GLFW_LIBRARY_QUERY();
            check("vulkan_unavailable",vulkanQuery!=0 && JNI.invokeI(vulkanQuery)==GLFW_FALSE);
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,2);
            glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT,GLFW_TRUE);
            window=glfwCreateWindow(1280,720,"MineSwitch Java LWJGL",0,0);
            check("real_window",window!=0);
            stressClosures(window);
            int afterStress=errors.get();
            glfwInitHint(0x7ffffffe,0);
            check("ffi_existing_callback_survives_reuse",errors.get()==afterStress+1);
            glfwGetError(null);
            final long callbackWindow=window;
            AtomicBoolean coordinates=new AtomicBoolean();
            GLFWCursorPosCallback position=GLFWCursorPosCallback.create((handle,x,y) ->
                coordinates.set(handle==callbackWindow && x==-2.75 && y==1.25));
            try {
                // A real typed C invocation of the executable libffi callback,
                // with pointer + two FP-register arguments; this is not an HID test.
                JNI.invokePV(window,-2.75,1.25,position.address());
                check("ffi_pointer_double_callback",coordinates.get());
            } finally { position.free(); }
            glfwMakeContextCurrent(window);
            glfwSwapInterval(1);
            GLCapabilities capabilities=GL.createCapabilities();
            glReady=true;
            System.out.println("GL_VERSION=" + glGetString(GL_VERSION));
            System.out.println("GL_RENDERER=" + glGetString(GL_RENDERER));
            System.out.println("GLSL_VERSION=" + glGetString(GL_SHADING_LANGUAGE_VERSION));
            check("minecraft_opengl32",capabilities.OpenGL32);
            imageDecode();
            vertex=shader(GL_VERTEX_SHADER,"#version 150 core\nin vec2 position; void main(){gl_Position=vec4(position,0.0,1.0);}");
            fragment=shader(GL_FRAGMENT_SHADER,"#version 150 core\nout vec4 color; void main(){color=vec4(0.2,0.7,0.4,1.0);}");
            program=glCreateProgram(); glAttachShader(program,vertex); glAttachShader(program,fragment);
            glBindAttribLocation(program,0,"position"); glLinkProgram(program);
            if (glGetProgrami(program,GL_LINK_STATUS)==GL_FALSE)
                throw new AssertionError("Program link: " + glGetProgramInfoLog(program));
            check("glsl150_shader_program",true);
            vao=glGenVertexArrays(); glBindVertexArray(vao);
            vertices=glGenBuffers(); glBindBuffer(GL_ARRAY_BUFFER,vertices);
            glBufferData(GL_ARRAY_BUFFER,new float[]{-0.8f,-0.8f,0.0f,0.8f,0.8f,-0.8f},GL_STATIC_DRAW);
            glVertexAttribPointer(0,2,GL_FLOAT,false,0,0L); glEnableVertexAttribArray(0);
            int width,height;
            try (MemoryStack stack=MemoryStack.stackPush()) {
                IntBuffer w=stack.mallocInt(1),h=stack.mallocInt(1);
                glfwGetFramebufferSize(window,w,h); width=w.get(0); height=h.get(0);
            }
            check("framebuffer_size",width>0 && height>0);
            glViewport(0,0,width,height);
            for (int frame=0;frame<90;frame++) {
                glClearColor(0.05f,0.08f,0.12f,1.0f); glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program); glDrawArrays(GL_TRIANGLES,0,3);
                if (frame==0) {
                    glFinish(); pixel(4,4,13,20,31); pixel(width/2,height/2,51,179,102);
                    check("gl_error_clear",glGetError()==GL_NO_ERROR);
                }
                glfwSwapBuffers(window); glfwPollEvents();
            }
            System.out.println("MINESWITCH_LWJGL_PASS checks=" + checks + " frames=90");
        } finally {
            if (program!=0) glDeleteProgram(program);
            if (vertex!=0) glDeleteShader(vertex);
            if (fragment!=0) glDeleteShader(fragment);
            if (vertices!=0) glDeleteBuffers(vertices);
            if (vao!=0) glDeleteVertexArrays(vao);
            if (window!=0) { if (glReady) GL.setCapabilities(null); glfwDestroyWindow(window); }
            if (initialized) glfwTerminate();
            if (callbackInstalled) glfwSetErrorCallback(null);
            errorCallback.free();
        }
    }
}
