# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep JNI bridge classes (PluginInfo, PortInfo, ScalePoint constructed via JNI reflection)
-keep class com.varcain.guitarrackcraft.engine.** { *; }

# Keep activities referenced in AndroidManifest
-keep class com.varcain.guitarrackcraft.MainActivity { *; }
-keep class com.varcain.guitarrackcraft.X11PluginUIActivity { *; }

# Keep Kotlin enums (used in when-expressions, serialization)
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

# Keep Compose runtime classes
-keep class androidx.compose.** { *; }
-dontwarn androidx.compose.**

# ── R8 / Gson ──
# Tone3000 数据模型（Session/User/Tone/Model 等）由 Gson 反射反序列化：
# 字段名被混淆会导致解析失败，需要保留类名与字段。
-keep class com.varcain.guitarrackcraft.ui.tone3000.** { *; }
# Gson TypeToken 泛型签名
-keepattributes Signature, InnerClasses, EnclosingMethod, *Annotation*, RuntimeVisibleAnnotations, RuntimeVisibleParameterAnnotations, AnnotationDefault

# ── VST host (full flavor) ──
# :vsthost_lib 的 prefab/native 桥接与 VST 安装流程中的 JNI 回调
-keep class com.varcain.vsthost.** { *; }
-keep class com.varcain.guitarrackcraft.ui.vst.** { *; }
-keep class com.varcain.guitarrackcraft.debug.** { *; }

# OkHttp / Conscrypt（ okhttp 自带 consumer 规则，这里仅压制残余警告）
-dontwarn okhttp3.internal.platform.**
-dontwarn org.conscrypt.**
-dontwarn org.bouncycastle.**
-dontwarn org.openjsse.**
