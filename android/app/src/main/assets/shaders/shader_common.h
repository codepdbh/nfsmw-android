#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// NFSMW (16/09): las constantes llegan por UBO dinamico (banco de constantes en Maxwell) en vez de
// leerse por puntero de 64 bits. Bit alto para no chocar con los de UNLEASHED_RECOMP.
#define SPEC_CONSTANT_CONSTANTES_UBO    (1 << 8)
// 18/09 (build 164): el desplazamiento del tfetch se divide por 1/tamano de las constantes compartidas en vez
// de preguntarselo a la textura. Una consulta de tamano es otra operacion de la unidad de texturas, y en la
// biblioteca hay 160 para 435 muestreos.
#define SPEC_CONSTANT_INV_TAMANO_TEX    (1 << 9)
// 20/09: PCF BARATO. Los shaders que muestrean el mapa de sombras lo hacen con un patron 3x3 a medio
// texel: nueve muestreos por pixel. En p_000101 -el humo y la salpicadura de las ruedas, que son
// rectangulos de PANTALLA COMPLETA- son once, y ese solo dibujo es el 21 % de la escena. Con este bit
// los ocho desplazamientos exteriores se ponen a cero: los nueve muestreos quedan identicos, DXC los
// funde en uno y la sombra pasa de filtrada 3x3 a un solo texel. Se pierde suavidad de borde.
#define SPEC_CONSTANT_PCF_BARATO        (1 << 15)
/* 20/09: LA FUNCION DE LA PRUEBA DE ALFA, ESPECIALIZADA (bits 16-18).
 *
 * g_AlphaFunction venia de las constantes compartidas, o sea en tiempo de ejecucion, asi que
 * alphaTestValue era un switch de 7 casos: ~8 ramas en TODOS los pixel shaders que hacen prueba de
 * alfa. En p_000131 -el mundo lejano, el 45 % de los fragmentos de la escena- eran 12 ramas para 1
 * muestreo y 14 operaciones. Y no hacia ninguna falta: la app ya conoce la funcion al crear el
 * pipeline (sale de RB_COLORCONTROL, que ya esta en la clave), asi que con tres bits mas el
 * compilador se queda con la unica comparacion que toca y tira las otras seis. La salida es
 * identica bit a bit. */
/* 20/09 (tarde): EL DESENFOQUE RADIAL DE LA COMPOSICION FINAL.
 *
 * p_000139 es el VisualTreatment de carrera: UN cuadrilatero a pantalla completa, 1280x720 exactos,
 * sin sobredibujo, con DOCE muestreos. Son 2,8-3,2 ms reales, el 72-80 % de todo el posproceso.
 *
 * De esos doce, SIETE son taps de DIFFUSEMAP desplazados (mas uno de HEIGHTMAP que solo alimenta el
 * factor): el desenfoque radial de velocidad. Se mezclan con `r5*factor + r3`, donde r3 es el tap
 * central, asi que con factor 0 la salida es EXACTAMENTE el tap central y los ocho muestreos
 * quedan muertos: DXC y el driver los borran al especializar.
 *
 * Se pierde el desenfoque de los bordes al acelerar y con el NOS. La imagen queda mas nitida. */
#define SPEC_CONSTANT_SIN_DESENFOQUE    (1 << 19)
/* 25/09 (build 184): EL MAPA DE SOMBRAS DEL MUNDO COMO MINIMO DE DOS TEXTURAS (nfsmw_nativo_sombra_minimo).
 *
 * El juego resuelve el mismo mapa de 1600x1600 dos veces: sin coches (lo muestrea la carroceria) y con los coches
 * dibujados encima (lo muestrea el mundo). El renderizador dibuja ahora los coches sobre un destino borrado a 1,0, sin
 * copiar debajo el mundo, y los shaders que muestrean el mapa con coches se quedan con el minimo de las dos texturas:
 * con la prueba de profundidad LESS/LEQUAL del juego, min(mundo, coches) es exactamente lo que deja dibujar los coches
 * encima. La segunda textura llega en la palabra del indice 3D del mismo registro (el mapa de sombras es 2D: esa
 * palabra no se usa). Con el bit apagado el codigo es el de tfetch2DSombra. */
#define SPEC_CONSTANT_SOMBRA_MINIMO     (1 << 23)
/* La app sabe que la biblioteca trae tfetch2DSombraMin porque esta constante aparece en el SPIR-V de los shaders que la
 * usan (OpConstant) y en ningun otro sitio. La constante de especializacion no llega nunca a este valor (bits 24-30). */
#define NFSMW_MARCA_SOMBRA_MINIMO       0x5E3B1A84u
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

// NFSMW (Vulkan 1.1): sin punteros de 64 bits. Las constantes llegan siempre por los UBO del conjunto 4,
// asi que los tres campos solo conservan el tamano del bloque de push constants (24 bytes) y la rama de
// vk::RawBufferLoad de las macros del traductor queda muerta: el SPIR-V no necesita Int64 ni buffer device address.
struct PushConstants
{
    uint2 VertexShaderConstants;
    uint2 PixelShaderConstants;
    uint2 SharedConstants;
};
namespace vk
{
    template<typename T> T NfsmwSinPuntero(uint2 direccion) { return (T)0; }
    template<typename T> T NfsmwSinPuntero(uint2 direccion, uint alineacion) { return (T)0; }
}
#define RawBufferLoad NfsmwSinPuntero

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

// NFSMW (16/09): los mismos bloques del bufer de subida, tambien como UBO dinamicos del conjunto 4.
// Con -fvk-use-dx-layout cada float4 ocupa 16 bytes contiguos, asi que cualquier palabra de 4 bytes del
// bloque compartido es un componente: v[B / 16][(B % 16) / 4], y asuint la lee sin tocar un bit.
struct NfsmwBloqueVs { float4 v[256]; };
struct NfsmwBloquePs { float4 v[224]; };
struct NfsmwBloqueCompartidas { float4 v[23]; };
[[vk::binding(0, 4)]] ConstantBuffer<NfsmwBloqueVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<NfsmwBloquePs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<NfsmwBloqueCompartidas> g_UboCompartidas;
#define NFSMW_UBO true
#define NFSMW_COMPARTIDA_UINT(B)  asuint(g_UboCompartidas.v[(B) / 16][((B) % 16) / 4])
#define NFSMW_COMPARTIDA_FLOAT(B) g_UboCompartidas.v[(B) / 16][((B) % 16) / 4]

#define g_Booleans                 (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(256) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256))
#define g_SwappedTexcoords         (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(260) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(264), NFSMW_COMPARTIDA_FLOAT(268)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (NFSMW_UBO ? NFSMW_COMPARTIDA_FLOAT(272) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272))
// NFSMW: funcion de la prueba de alfa (RB_COLORCONTROL.alpha_func): 0 nunca, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 siempre.
#define g_AlphaFunction            (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(276) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276))
// NFSMW: posicion al espacio de recorte del host, como ndc_scale/ndc_offset de
// la emulacion (graphics/util/draw.cpp). (1, 1) y (0, 0) en los dibujos normales;
// con el recorte del Xenos desactivado pasa de pixeles a NDC.
#define g_NdcScale                 (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(280), NFSMW_COMPARTIDA_FLOAT(284)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(288), NFSMW_COMPARTIDA_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))
// NFSMW: de donde sale cada componente de la entrada de vertices de esa ubicacion
// (el D3D parchea el swizzle del fetch segun la declaracion). 0xFFF = tal cual.
#define g_InputRemap(LOC)          (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(296 + (LOC) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 296 + (LOC) * 4))

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF

#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // 18/09 (build 164): con el bit puesto, el tamano viene por constante y no hay que preguntarselo a la
    // textura. Es la misma cuenta con el mismo numero: el renderizador escribe 1/tamano de la imagen del host.
    // Con un ternario, DXC evalua las dos ramas y la consulta de tamano se queda igual: hace falta un if con
    // [branch] para que la rama muerta desaparezca al especializar el pipeline.
    float2 desplazamiento;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)
        desplazamiento = offset * invSize;
    else
        desplazamiento = offset / getTexture2DDimensions(texture);
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + desplazamiento);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

// 20/09: el muestreo del MAPA DE SOMBRAS. El traductor emite tfetch2D para todo; el paso de
// reescritura de la biblioteca cambia a esta funcion las llamadas cuyo muestreador es SHADOWMAP_SAMPLER,
// que son las unicas que queremos poder abaratar. Con el bit puesto, todos los muestreos del 3x3 caen
// en el mismo texel y el compilador se queda con uno solo.
float4 tfetch2DSombra(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_BARATO)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
}

// 25/09 (build 184): el mapa de sombras con el minimo de su pareja (nfsmw_nativo_sombra_minimo). El paso de la biblioteca
// cambia a esta funcion todas las llamadas al mapa de sombras y le pasa el indice 3D de ese mismo registro, que es donde
// la app pone la pareja: el mapa del mundo (o la propia textura, que da el mismo texel, mientras la app vigila). Las dos
// se muestrean igual (mismo sampler puntual, mismas coordenadas y desplazamientos), asi que el minimo es por texel. La
// primera comparacion es la marca de la biblioteca: no se cumple nunca y el driver la borra al especializar.
float4 tfetch2DSombraMin(uint resourceDescriptorIndex, uint parejaDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == NFSMW_MARCA_SOMBRA_MINIMO)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 valor = tfetch2DSombra(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SOMBRA_MINIMO)
        valor = min(valor, tfetch2DSombra(parejaDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize));
    return valor;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (semanticIndex < 32 && (swappedTexcoords & (1u << semanticIndex)) != 0) ? value.yxwz : value;
}

// NFSMW: 3 bits por componente: 0-3 = componente del dato, 4 = 0, 5 = 1, 7 = el mismo.
float4 remapInput(float4 value, uint code)
{
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// NFSMW: prueba de alfa del Xenos con su funcion de comparacion. Positivo si el pixel pasa.
float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
