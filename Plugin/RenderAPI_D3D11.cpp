#include "RenderAPI.h"
#include "PlatformBase.h"

// Direct3D 11 implementation of RenderAPI.

// #if SUPPORT_D3D11

#include <assert.h>
#include <tchar.h>
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include "Unity/IUnityGraphicsD3D11.h"
#include "Log.h"

#include <algorithm>
#include <dxgi1_2.h>
#include <comdef.h>
#include <mingw.mutex.h>

#define SCREEN_WIDTH  100
#define SCREEN_HEIGHT  100
#define BORDER_LEFT    (-0.95f)
#define BORDER_RIGHT   ( 0.85f)
#define BORDER_TOP     ( 0.95f)
#define BORDER_BOTTOM  (-0.90f)

class TextureBuffer {
    public:
    ID3D11Texture2D          *m_textureUnity         = nullptr;
    ID3D11ShaderResourceView *m_textureShaderInput   = nullptr;
    HANDLE                   m_sharedHandle          = nullptr; // handle of the texture used by VLC and the app
    ID3D11RenderTargetView   *m_textureRenderTarget  = nullptr;
};


class RenderAPI_D3D11 : public RenderAPI
{
public:
    virtual void setVlcContext(libvlc_media_player_t *mp) override;
	virtual void ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces) override;
    void* getVideoFrame(bool* out_updated) override;

    /* VLC callbacks */
    bool UpdateOutput( const libvlc_video_direct3d_cfg_t *cfg, libvlc_video_output_cfg_t *out );
    void Swap();
    bool StartRendering(bool enter, const libvlc_video_direct3d_hdr10_metadata_t *hdr10 );
    bool SelectPlane(size_t plane );
    bool Setup(const libvlc_video_direct3d_device_cfg_t *cfg, libvlc_video_direct3d_device_setup_t *out );
    void Cleanup();
    void Resize(void (*report_size_change)(void *report_opaque, unsigned width, unsigned height), void *report_opaque );

private:
	void CreateResources(ID3D11Device *d3device, ID3D11DeviceContext *d3dctx);
	void ReleaseResources();
    void ReleaseBuffer(TextureBuffer* buffer);
    void InitBuffer(TextureBuffer* buffer);
    void DebugInUnity(LPCSTR message);
    void Update(UINT width, UINT height);

    /* Unity side resources */
	ID3D11Device             *m_d3deviceUnity       = nullptr;
	ID3D11DeviceContext      *m_d3dctxUnity         = nullptr;

    /* VLC side resources */
    ID3D11Device            *m_d3deviceVLC          = nullptr;
    ID3D11DeviceContext     *m_d3dctxVLC            = nullptr;

    /* Texture buffers */
    TextureBuffer           *m_frontBuffer          = nullptr;
    TextureBuffer           *m_backBuffer           = nullptr;

    CRITICAL_SECTION m_sizeLock; // the ReportSize callback cannot be called during/after the Cleanup_cb is called
    unsigned m_width = 0;
    unsigned m_height = 0;
    void (*m_ReportSize)(void *ReportOpaque, unsigned width, unsigned height) = nullptr;
    void *m_reportOpaque = nullptr;

    bool m_updated = false;

    CRITICAL_SECTION m_outputLock; // the ReportSize callback cannot be called during/after the Cleanup_cb is called
};

// VLC C-style callbacks
bool UpdateOutput_cb( void *opaque, const libvlc_video_direct3d_cfg_t *cfg, libvlc_video_output_cfg_t *out )
{
    return ((RenderAPI_D3D11*)opaque)->UpdateOutput(cfg, out);
}

void Swap_cb( void* opaque )
{
    ((RenderAPI_D3D11*)opaque)->Swap();
}

bool StartRendering_cb( void *opaque, bool enter, const libvlc_video_direct3d_hdr10_metadata_t *hdr10 )
{
    return ((RenderAPI_D3D11*)opaque)->StartRendering(enter, hdr10);
}

bool SelectPlane_cb( void *opaque, size_t plane )
{
    return ((RenderAPI_D3D11*)opaque)->SelectPlane(plane);
}

bool Setup_cb( void **opaque, const libvlc_video_direct3d_device_cfg_t *cfg, libvlc_video_direct3d_device_setup_t *out )
{
    return ((RenderAPI_D3D11*)*opaque)->Setup(cfg, out);
}

void Cleanup_cb( void *opaque )
{
    ((RenderAPI_D3D11*)opaque)->Cleanup();
}

void Resize_cb( void *opaque,
                    void (*report_size_change)(void *report_opaque, unsigned width, unsigned height),
                    void *report_opaque )
{
    ((RenderAPI_D3D11*)opaque)->Resize(report_size_change, report_opaque);
}

RenderAPI* CreateRenderAPI_D3D11()
{
	return new RenderAPI_D3D11();
}

void RenderAPI_D3D11::setVlcContext(libvlc_media_player_t *mp)
{
    DEBUG("[D3D11] setVlcContext %p", this);

    libvlc_video_direct3d_set_callbacks( mp, libvlc_video_direct3d_engine_d3d11,
                                    Setup_cb, Cleanup_cb, Resize_cb, UpdateOutput_cb,
                                    Swap_cb, StartRendering_cb, SelectPlane_cb,
                                    this);
}

void RenderAPI_D3D11::ProcessDeviceEvent(UnityGfxDeviceEventType type, IUnityInterfaces* interfaces)
{
    DEBUG("Entering ProcessDeviceEvent \n");

	switch (type)
	{
        case kUnityGfxDeviceEventInitialize:
        {
            IUnityGraphicsD3D11* d3d = interfaces->Get<IUnityGraphicsD3D11>();
            if(d3d == NULL)
            {
                DEBUG("Could not retrieve IUnityGraphicsD3D11 \n");
                return;
            }
            ID3D11Device* d3device = d3d->GetDevice();
            if(d3device == NULL)
            {
                DEBUG("Could not retrieve d3device \n");
                return;
            }
            ID3D11DeviceContext* d3dctx;
            d3device->GetImmediateContext(&d3dctx);
            if(d3dctx == NULL)
            {
                DEBUG("Could not retrieve d3dctx \n");
                return;
            }
            CreateResources(d3device, d3dctx);
            break;
        }
        case kUnityGfxDeviceEventShutdown:
        {
            ReleaseResources();
            break;
        }
        case kUnityGfxDeviceEventAfterReset:
        {
            break;
        }
        case kUnityGfxDeviceEventBeforeReset:
        {
            break;
        }
    }
}

void RenderAPI_D3D11::Update(UINT width, UINT height)
{
    DEBUG("start releasing d3d objects.\n");
    EnterCriticalSection(&m_outputLock);

    m_width = width;
    m_height = height;

    ReleaseBuffer(m_frontBuffer);
    ReleaseBuffer(m_backBuffer);

    DEBUG("Done releasing d3d objects.\n");

    InitBuffer(m_frontBuffer);
    InitBuffer(m_backBuffer);
    LeaveCriticalSection(&m_outputLock);
}

void RenderAPI_D3D11::CreateResources(ID3D11Device *d3device, ID3D11DeviceContext *d3dctx)
{
    DEBUG("Entering CreateResources \n");

 	HRESULT hr;

    ZeroMemory(&m_sizeLock, sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(&m_sizeLock);
    ZeroMemory(&m_outputLock, sizeof(CRITICAL_SECTION));
    InitializeCriticalSection(&m_outputLock);

    m_d3deviceUnity = d3device;
    m_d3dctxUnity = d3dctx;

    m_frontBuffer = new TextureBuffer;
    m_backBuffer = new TextureBuffer;

    UINT creationFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT; /* needed for hardware decoding */
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG; //TODO: remove for release mode

    hr = D3D11CreateDevice(NULL,
                        D3D_DRIVER_TYPE_HARDWARE,
                        NULL,
                        creationFlags,
                        NULL,
                        NULL,
                        D3D11_SDK_VERSION,
                        &m_d3deviceVLC,
                        NULL,
                        &m_d3dctxVLC);
    DEBUG("CreateResources m_d3dctxVLC = %p this = %p", m_d3dctxVLC, this);

    if(FAILED(hr))
    {
        DEBUG("FAILED to create d3d11 device and context \n");
        abort();
    }

    DEBUG("Configuring multithread \n");

    /* The ID3D11Device must have multithread protection */
    ID3D10Multithread *pMultithread;
    hr = m_d3deviceUnity->QueryInterface(&pMultithread);
    if (SUCCEEDED(hr)) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
    }

    Update(SCREEN_WIDTH, SCREEN_HEIGHT);
    DEBUG("Exiting CreateResources.\n");
}

void RenderAPI_D3D11::ReleaseResources()
{
    DEBUG("Entering ReleaseResources.\n");
    if(m_frontBuffer != nullptr)
    {
        ReleaseBuffer(m_frontBuffer);
        delete m_frontBuffer;
        m_frontBuffer = nullptr;
    }

    if(m_backBuffer != nullptr)
    {
        ReleaseBuffer(m_backBuffer);
        delete m_backBuffer;
        m_backBuffer = nullptr;
    }

    if(m_d3deviceVLC)
    {
        m_d3deviceVLC->Release();
        m_d3deviceVLC = nullptr;
    }
    
    if(m_d3dctxVLC)
    {
        m_d3dctxVLC->Release();
        m_d3deviceVLC = nullptr;
    }
}

void RenderAPI_D3D11::ReleaseBuffer(TextureBuffer* buffer)
{
    if(buffer->m_textureRenderTarget)
    {
        buffer->m_textureRenderTarget->Release();
        buffer->m_textureRenderTarget = nullptr;
    }
        
    if(buffer->m_textureShaderInput)
    {
        buffer->m_textureShaderInput->Release();
        buffer->m_textureShaderInput = nullptr;
    }

    if(buffer->m_textureUnity)
    {
        buffer->m_textureUnity->Release();
        buffer->m_textureUnity = nullptr;
    }

    if(buffer->m_sharedHandle)
    {
        CloseHandle(buffer->m_sharedHandle);
        buffer->m_sharedHandle = nullptr;
    }
}

void RenderAPI_D3D11::InitBuffer( TextureBuffer* buffer )
{
    DXGI_FORMAT renderFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    HRESULT hr;

    /* interim texture */
    D3D11_TEXTURE2D_DESC texDesc = { 0 };
    texDesc.MipLevels = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.CPUAccessFlags = 0;
    texDesc.ArraySize = 1;
    texDesc.Format = renderFormat;
    texDesc.Height = m_height;
    texDesc.Width  = m_width;
    
    hr = m_d3deviceUnity->CreateTexture2D( &texDesc, NULL, &buffer->m_textureUnity );
    if (FAILED(hr))
    {
        DEBUG("CreateTexture2D FAILED \n");
    }
    else
    {
        DEBUG("CreateTexture2D SUCCEEDED.\n");
    }

    IDXGIResource1* sharedResource = NULL;

    hr = buffer->m_textureUnity->QueryInterface(&sharedResource);
    if(FAILED(hr))
    {
        DEBUG("get IDXGIResource1 FAILED \n");
        abort();
    }

    hr = sharedResource->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &buffer->m_sharedHandle);
    if(FAILED(hr))
    {
        _com_error error(hr);
        DEBUG("sharedResource->CreateSharedHandle FAILED %s \n", error.ErrorMessage());
        abort();
    }

    sharedResource->Release();

    ID3D11Device1* d3d11VLC1;
    hr = m_d3deviceVLC->QueryInterface(&d3d11VLC1);
    if(FAILED(hr))
    {
        _com_error error(hr);
        DEBUG("QueryInterface ID3D11Device1 FAILED %s \n", error.ErrorMessage());
        abort();
    }
    
    ID3D11Texture2D* textureVLC;
    hr = d3d11VLC1->OpenSharedResource1(buffer->m_sharedHandle, __uuidof(ID3D11Texture2D), (void**)&textureVLC);
    if(FAILED(hr))
    {
        _com_error error(hr);
        DEBUG("ctx->d3device->OpenSharedResource FAILED %s \n", error.ErrorMessage());
        abort();
    }

    d3d11VLC1->Release();

    D3D11_SHADER_RESOURCE_VIEW_DESC resviewDesc;
    ZeroMemory(&resviewDesc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
    resviewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    resviewDesc.Texture2D.MipLevels = 1;
    resviewDesc.Format = texDesc.Format;
    hr = m_d3deviceUnity->CreateShaderResourceView(buffer->m_textureUnity, &resviewDesc, &buffer->m_textureShaderInput);
    if (FAILED(hr))
    {
        DEBUG("CreateShaderResourceView FAILED \n");
    }
    else
    {
        DEBUG("CreateShaderResourceView SUCCEEDED.\n");
    }

    D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc;
    ZeroMemory(&renderTargetViewDesc, sizeof(D3D11_RENDER_TARGET_VIEW_DESC));
    renderTargetViewDesc.Format = texDesc.Format;
    renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    hr = m_d3deviceVLC->CreateRenderTargetView(textureVLC, &renderTargetViewDesc, &buffer->m_textureRenderTarget);
    if (FAILED(hr))
    {
        DEBUG("CreateRenderTargetView FAILED \n");
    }

    textureVLC->Release();// No need to keep a reference to that, VLC only writes to the renderTarget
}

bool RenderAPI_D3D11::UpdateOutput( const libvlc_video_direct3d_cfg_t *cfg, libvlc_video_output_cfg_t *out )
{
    DEBUG("Entering UpdateOutput_cb.\n");

    DXGI_FORMAT renderFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    Update(cfg->width, cfg->height);

    out->surface_format = renderFormat;
    out->full_range     = true;
    out->colorspace     = libvlc_video_colorspace_BT709;
    out->primaries      = libvlc_video_primaries_BT709;
    out->transfer       = libvlc_video_transfer_func_LINEAR;

    DEBUG("Exiting UpdateOutput_cb \n");

    return true;
}

void RenderAPI_D3D11::Swap()
{
    EnterCriticalSection(&m_outputLock);
    m_d3dctxVLC->Flush();
    std::swap(m_frontBuffer, m_backBuffer);
    m_updated = true;
    LeaveCriticalSection(&m_outputLock);
}

bool RenderAPI_D3D11::StartRendering( bool enter, const libvlc_video_direct3d_hdr10_metadata_t *hdr10 )
{
    if ( enter )
    {
        static const FLOAT blackRGBA[4] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_d3dctxVLC->OMSetRenderTargets(1, &m_backBuffer->m_textureRenderTarget, NULL);
        m_d3dctxVLC->ClearRenderTargetView( m_backBuffer->m_textureRenderTarget, blackRGBA);
        return true;
    }

    return true;
}

bool RenderAPI_D3D11::SelectPlane( size_t plane )
{
    if ( plane != 0 ) // we only support one packed RGBA plane (DXGI_FORMAT_R8G8B8A8_UNORM)
        return false;
    m_d3dctxVLC->OMSetRenderTargets( 1, &m_backBuffer->m_textureRenderTarget, NULL );
    return true;
}

bool RenderAPI_D3D11::Setup( const libvlc_video_direct3d_device_cfg_t *cfg, libvlc_video_direct3d_device_setup_t *out )
{
    out->device_context = m_d3dctxVLC;
    return true;
}

void RenderAPI_D3D11::Cleanup()
{
    // here we can release all things Direct3D11 for good (if playing only one file)
}

void RenderAPI_D3D11::Resize(void (*report_size_change)(void *report_opaque, unsigned width, unsigned height),
                       void *report_opaque )
{
    DEBUG("Resize_cb called \n");
    EnterCriticalSection(&m_sizeLock);
    m_ReportSize = report_size_change;
    m_reportOpaque = report_opaque;

    if (m_ReportSize != nullptr)
    {
        DEBUG("Invoking m_ReportSize(m_reportOpaque, m_width, m_height) with width=%u and height=%u \n", m_width, m_height);

        /* report our initial size */
        m_ReportSize(m_reportOpaque, m_width, m_height);
    }
    LeaveCriticalSection(&m_sizeLock);

    DEBUG("Exiting Resize_cb");
}

void* RenderAPI_D3D11::getVideoFrame(bool* out_updated)
{
    EnterCriticalSection(&m_outputLock);
    *out_updated = m_updated;
    
    LeaveCriticalSection(&m_outputLock);
    return m_frontBuffer->m_textureShaderInput;
}



// #endif // #if SUPPORT_D3D11
