#include "log.hpp"
#include "d3d10_runtime.hpp"
#include "d3d10_fx_compiler.hpp"
#include "lexer.hpp"
#include "input.hpp"
#include <imgui.h>

namespace reshade
{
	namespace
	{
		DXGI_FORMAT make_format_srgb(DXGI_FORMAT format)
		{
			switch (format)
			{
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_R8G8B8A8_UNORM:
					return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				case DXGI_FORMAT_BC1_TYPELESS:
				case DXGI_FORMAT_BC1_UNORM:
					return DXGI_FORMAT_BC1_UNORM_SRGB;
				case DXGI_FORMAT_BC2_TYPELESS:
				case DXGI_FORMAT_BC2_UNORM:
					return DXGI_FORMAT_BC2_UNORM_SRGB;
				case DXGI_FORMAT_BC3_TYPELESS:
				case DXGI_FORMAT_BC3_UNORM:
					return DXGI_FORMAT_BC3_UNORM_SRGB;
				default:
					return format;
			}
		}
		DXGI_FORMAT make_format_normal(DXGI_FORMAT format)
		{
			switch (format)
			{
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
					return DXGI_FORMAT_R8G8B8A8_UNORM;
				case DXGI_FORMAT_BC1_TYPELESS:
				case DXGI_FORMAT_BC1_UNORM_SRGB:
					return DXGI_FORMAT_BC1_UNORM;
				case DXGI_FORMAT_BC2_TYPELESS:
				case DXGI_FORMAT_BC2_UNORM_SRGB:
					return DXGI_FORMAT_BC2_UNORM;
				case DXGI_FORMAT_BC3_TYPELESS:
				case DXGI_FORMAT_BC3_UNORM_SRGB:
					return DXGI_FORMAT_BC3_UNORM;
				default:
					return format;
			}
		}
		DXGI_FORMAT make_format_typeless(DXGI_FORMAT format)
		{
			switch (format)
			{
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
					return DXGI_FORMAT_R8G8B8A8_TYPELESS;
				case DXGI_FORMAT_BC1_UNORM:
				case DXGI_FORMAT_BC1_UNORM_SRGB:
					return DXGI_FORMAT_BC1_TYPELESS;
				case DXGI_FORMAT_BC2_UNORM:
				case DXGI_FORMAT_BC2_UNORM_SRGB:
					return DXGI_FORMAT_BC2_TYPELESS;
				case DXGI_FORMAT_BC3_UNORM:
				case DXGI_FORMAT_BC3_UNORM_SRGB:
					return DXGI_FORMAT_BC3_TYPELESS;
				default:
					return format;
			}
		}

		UINT get_renderer_id(ID3D10Device *device)
		{
			com_ptr<ID3D10Device1> device1;

			if (SUCCEEDED(device->QueryInterface(&device1)))
			{
				return device1->GetFeatureLevel();
			}
			else
			{
				return D3D10_FEATURE_LEVEL_10_0;
			}
		}
	}

	d3d10_runtime::d3d10_runtime(ID3D10Device *device, IDXGISwapChain *swapchain) : runtime(get_renderer_id(device)), _device(device), _swapchain(swapchain), _stateblock(device)
	{
		assert(device != nullptr);
		assert(swapchain != nullptr);

		HRESULT hr;
		DXGI_ADAPTER_DESC adapter_desc;
		com_ptr<IDXGIDevice> dxgidevice;
		com_ptr<IDXGIAdapter> dxgiadapter;

		hr = _device->QueryInterface(&dxgidevice);

		assert(SUCCEEDED(hr));

		hr = dxgidevice->GetAdapter(&dxgiadapter);

		assert(SUCCEEDED(hr));

		hr = dxgiadapter->GetDesc(&adapter_desc);

		assert(SUCCEEDED(hr));
			
		_vendor_id = adapter_desc.VendorId;
		_device_id = adapter_desc.DeviceId;
	}

	bool d3d10_runtime::init_backbuffer_texture()
	{
		HRESULT hr = _swapchain->GetBuffer(0, __uuidof(ID3D10Texture2D), reinterpret_cast<void **>(&_backbuffer));

		assert(SUCCEEDED(hr));

		D3D10_TEXTURE2D_DESC texdesc;
		texdesc.Width = _width;
		texdesc.Height = _height;
		texdesc.ArraySize = texdesc.MipLevels = 1;
		texdesc.Format = make_format_typeless(_backbuffer_format);
		texdesc.SampleDesc.Count = 1;
		texdesc.SampleDesc.Quality = 0;
		texdesc.Usage = D3D10_USAGE_DEFAULT;
		texdesc.BindFlags = D3D10_BIND_RENDER_TARGET;
		texdesc.MiscFlags = texdesc.CPUAccessFlags = 0;

		OSVERSIONINFOEX verinfo_windows7 = { sizeof(OSVERSIONINFOEX), 6, 1 };
		const bool is_windows7 = VerifyVersionInfo(&verinfo_windows7, VER_MAJORVERSION | VER_MINORVERSION, VerSetConditionMask(VerSetConditionMask(0, VER_MAJORVERSION, VER_EQUAL), VER_MINORVERSION, VER_EQUAL)) != FALSE;

		if (_is_multisampling_enabled || make_format_normal(_backbuffer_format) != _backbuffer_format || !is_windows7)
		{
			hr = _device->CreateTexture2D(&texdesc, nullptr, &_backbuffer_resolved);

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create back buffer replacement (Width = " << texdesc.Width << ", Height = " << texdesc.Height << ", Format = " << texdesc.Format << ", SampleCount = " << texdesc.SampleDesc.Count << ", SampleQuality = " << texdesc.SampleDesc.Quality << ")! HRESULT is '" << std::hex << hr << std::dec << "'.";
				return false;
			}

			hr = _device->CreateRenderTargetView(_backbuffer.get(), nullptr, &_backbuffer_rtv[2]);

			assert(SUCCEEDED(hr));
		}
		else
		{
			_backbuffer_resolved = _backbuffer;
		}

		// Create back buffer shader texture
		texdesc.BindFlags = D3D10_BIND_SHADER_RESOURCE;

		hr = _device->CreateTexture2D(&texdesc, nullptr, &_backbuffer_texture);

		if (SUCCEEDED(hr))
		{
			D3D10_SHADER_RESOURCE_VIEW_DESC srvdesc = { };
			srvdesc.Format = make_format_normal(texdesc.Format);
			srvdesc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
			srvdesc.Texture2D.MipLevels = texdesc.MipLevels;

			if (SUCCEEDED(hr))
			{
				hr = _device->CreateShaderResourceView(_backbuffer_texture.get(), &srvdesc, &_backbuffer_texture_srv[0]);
			}
			else
			{
				LOG(TRACE) << "Failed to create back buffer texture resource view (Format = " << srvdesc.Format << ")! HRESULT is '" << std::hex << hr << std::dec << "'.";
			}

			srvdesc.Format = make_format_srgb(texdesc.Format);

			if (SUCCEEDED(hr))
			{
				hr = _device->CreateShaderResourceView(_backbuffer_texture.get(), &srvdesc, &_backbuffer_texture_srv[1]);
			}
			else
			{
				LOG(TRACE) << "Failed to create back buffer SRGB texture resource view (Format = " << srvdesc.Format << ")! HRESULT is '" << std::hex << hr << std::dec << "'.";
			}
		}
		else
		{
			LOG(TRACE) << "Failed to create back buffer texture (Width = " << texdesc.Width << ", Height = " << texdesc.Height << ", Format = " << texdesc.Format << ", SampleCount = " << texdesc.SampleDesc.Count << ", SampleQuality = " << texdesc.SampleDesc.Quality << ")! HRESULT is '" << std::hex << hr << std::dec << "'.";
		}

		if (FAILED(hr))
		{
			return false;
		}

		D3D10_RENDER_TARGET_VIEW_DESC rtdesc = { };
		rtdesc.Format = make_format_normal(texdesc.Format);
		rtdesc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE2D;

		hr = _device->CreateRenderTargetView(_backbuffer_resolved.get(), &rtdesc, &_backbuffer_rtv[0]);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create back buffer render target (Format = " << rtdesc.Format << ")! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		rtdesc.Format = make_format_srgb(texdesc.Format);

		hr = _device->CreateRenderTargetView(_backbuffer_resolved.get(), &rtdesc, &_backbuffer_rtv[1]);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create back buffer SRGB render target (Format = " << rtdesc.Format << ")! HRESULT is '" << std::hex << hr << std::dec << "'.";

			return false;
		}

		{
			const BYTE vs[] = { 68, 88, 66, 67, 224, 206, 72, 137, 142, 185, 68, 219, 247, 216, 225, 132, 111, 78, 106, 20, 1, 0, 0, 0, 156, 2, 0, 0, 5, 0, 0, 0, 52, 0, 0, 0, 140, 0, 0, 0, 192, 0, 0, 0, 24, 1, 0, 0, 32, 2, 0, 0, 82, 68, 69, 70, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0, 0, 0, 0, 4, 254, 255, 0, 1, 0, 0, 28, 0, 0, 0, 77, 105, 99, 114, 111, 115, 111, 102, 116, 32, 40, 82, 41, 32, 72, 76, 83, 76, 32, 83, 104, 97, 100, 101, 114, 32, 67, 111, 109, 112, 105, 108, 101, 114, 32, 54, 46, 51, 46, 57, 54, 48, 48, 46, 49, 54, 51, 56, 52, 0, 171, 171, 73, 83, 71, 78, 44, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 83, 86, 95, 86, 101, 114, 116, 101, 120, 73, 68, 0, 79, 83, 71, 78, 80, 0, 0, 0, 2, 0, 0, 0, 8, 0, 0, 0, 56, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 3, 12, 0, 0, 83, 86, 95, 80, 111, 115, 105, 116, 105, 111, 110, 0, 84, 69, 88, 67, 79, 79, 82, 68, 0, 171, 171, 171, 83, 72, 68, 82, 0, 1, 0, 0, 64, 0, 1, 0, 64, 0, 0, 0, 96, 0, 0, 4, 18, 16, 16, 0, 0, 0, 0, 0, 6, 0, 0, 0, 103, 0, 0, 4, 242, 32, 16, 0, 0, 0, 0, 0, 1, 0, 0, 0, 101, 0, 0, 3, 50, 32, 16, 0, 1, 0, 0, 0, 104, 0, 0, 2, 1, 0, 0, 0, 32, 0, 0, 10, 50, 0, 16, 0, 0, 0, 0, 0, 6, 16, 16, 0, 0, 0, 0, 0, 2, 64, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 10, 50, 0, 16, 0, 0, 0, 0, 0, 70, 0, 16, 0, 0, 0, 0, 0, 2, 64, 0, 0, 0, 0, 0, 64, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 50, 0, 0, 15, 50, 32, 16, 0, 0, 0, 0, 0, 70, 0, 16, 0, 0, 0, 0, 0, 2, 64, 0, 0, 0, 0, 0, 64, 0, 0, 0, 192, 0, 0, 0, 0, 0, 0, 0, 0, 2, 64, 0, 0, 0, 0, 128, 191, 0, 0, 128, 63, 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 5, 50, 32, 16, 0, 1, 0, 0, 0, 70, 0, 16, 0, 0, 0, 0, 0, 54, 0, 0, 8, 194, 32, 16, 0, 0, 0, 0, 0, 2, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 128, 63, 62, 0, 0, 1, 83, 84, 65, 84, 116, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			hr = _device->CreateVertexShader(vs, ARRAYSIZE(vs), &_copy_vertex_shader);

			assert(SUCCEEDED(hr));
		}

		{
			const BYTE ps[] = { 68, 88, 66, 67, 93, 102, 148, 45, 34, 106, 51, 79, 54, 23, 136, 21, 27, 217, 232, 71, 1, 0, 0, 0, 116, 2, 0, 0, 5, 0, 0, 0, 52, 0, 0, 0, 208, 0, 0, 0, 40, 1, 0, 0, 92, 1, 0, 0, 248, 1, 0, 0, 82, 68, 69, 70, 148, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 28, 0, 0, 0, 0, 4, 255, 255, 0, 1, 0, 0, 98, 0, 0, 0, 92, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 95, 0, 0, 0, 2, 0, 0, 0, 5, 0, 0, 0, 4, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 1, 0, 0, 0, 13, 0, 0, 0, 115, 48, 0, 116, 48, 0, 77, 105, 99, 114, 111, 115, 111, 102, 116, 32, 40, 82, 41, 32, 72, 76, 83, 76, 32, 83, 104, 97, 100, 101, 114, 32, 67, 111, 109, 112, 105, 108, 101, 114, 32, 54, 46, 51, 46, 57, 54, 48, 48, 46, 49, 54, 51, 56, 52, 0, 73, 83, 71, 78, 80, 0, 0, 0, 2, 0, 0, 0, 8, 0, 0, 0, 56, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 3, 3, 0, 0, 83, 86, 95, 80, 111, 115, 105, 116, 105, 111, 110, 0, 84, 69, 88, 67, 79, 79, 82, 68, 0, 171, 171, 171, 79, 83, 71, 78, 44, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 83, 86, 95, 84, 97, 114, 103, 101, 116, 0, 171, 171, 83, 72, 68, 82, 148, 0, 0, 0, 64, 0, 0, 0, 37, 0, 0, 0, 90, 0, 0, 3, 0, 96, 16, 0, 0, 0, 0, 0, 88, 24, 0, 4, 0, 112, 16, 0, 0, 0, 0, 0, 85, 85, 0, 0, 98, 16, 0, 3, 50, 16, 16, 0, 1, 0, 0, 0, 101, 0, 0, 3, 242, 32, 16, 0, 0, 0, 0, 0, 104, 0, 0, 2, 1, 0, 0, 0, 69, 0, 0, 9, 242, 0, 16, 0, 0, 0, 0, 0, 70, 16, 16, 0, 1, 0, 0, 0, 70, 126, 16, 0, 0, 0, 0, 0, 0, 96, 16, 0, 0, 0, 0, 0, 54, 0, 0, 5, 114, 32, 16, 0, 0, 0, 0, 0, 70, 2, 16, 0, 0, 0, 0, 0, 54, 0, 0, 5, 130, 32, 16, 0, 0, 0, 0, 0, 1, 64, 0, 0, 0, 0, 128, 63, 62, 0, 0, 1, 83, 84, 65, 84, 116, 0, 0, 0, 4, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			hr = _device->CreatePixelShader(ps, ARRAYSIZE(ps), &_copy_pixel_shader);

			assert(SUCCEEDED(hr));
		}

		{
			const D3D10_SAMPLER_DESC copysampdesc = { D3D10_FILTER_MIN_MAG_MIP_POINT, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP, D3D10_TEXTURE_ADDRESS_CLAMP };
			hr = _device->CreateSamplerState(&copysampdesc, &_copy_sampler);

			assert(SUCCEEDED(hr));
		}

		return true;
	}
	bool d3d10_runtime::init_default_depth_stencil()
	{
		const D3D10_TEXTURE2D_DESC desc = {
			_width,
			_height,
			1,
			1,
			DXGI_FORMAT_D24_UNORM_S8_UINT,
			{ 1, 0 },
			D3D10_USAGE_DEFAULT,
			D3D10_BIND_DEPTH_STENCIL
		};

		com_ptr<ID3D10Texture2D> depth_stencil_texture;

		HRESULT hr = _device->CreateTexture2D(&desc, nullptr, &depth_stencil_texture);

		if (FAILED(hr))
		{
			return false;
		}

		hr = _device->CreateDepthStencilView(depth_stencil_texture.get(), nullptr, &_default_depthstencil);

		if (FAILED(hr))
		{
			return false;
		}

		return true;
	}
	bool d3d10_runtime::init_fx_resources()
	{
		D3D10_RASTERIZER_DESC desc = { };
		desc.FillMode = D3D10_FILL_SOLID;
		desc.CullMode = D3D10_CULL_NONE;
		desc.DepthClipEnable = TRUE;

		HRESULT hr = _device->CreateRasterizerState(&desc, &_effect_rasterizer_state);

		if (FAILED(hr))
		{
			return false;
		}

		return true;
	}
	bool d3d10_runtime::init_imgui_resources()
	{
		HRESULT hr;

		// Create the vertex shader
		{
			const BYTE vs[] = { 68, 88, 66, 67, 165, 101, 108, 186, 56, 122, 39, 81, 174, 124, 224, 24, 237, 222, 192, 228, 1, 0, 0, 0, 120, 3, 0, 0, 5, 0, 0, 0, 52, 0, 0, 0, 16, 1, 0, 0, 128, 1, 0, 0, 244, 1, 0, 0, 252, 2, 0, 0, 82, 68, 69, 70, 212, 0, 0, 0, 1, 0, 0, 0, 76, 0, 0, 0, 1, 0, 0, 0, 28, 0, 0, 0, 0, 4, 254, 255, 0, 1, 0, 0, 160, 0, 0, 0, 60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 118, 101, 114, 116, 101, 120, 66, 117, 102, 102, 101, 114, 0, 171, 171, 171, 60, 0, 0, 0, 1, 0, 0, 0, 100, 0, 0, 0, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 124, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0, 0, 2, 0, 0, 0, 144, 0, 0, 0, 0, 0, 0, 0, 80, 114, 111, 106, 101, 99, 116, 105, 111, 110, 77, 97, 116, 114, 105, 120, 0, 171, 171, 171, 3, 0, 3, 0, 4, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 77, 105, 99, 114, 111, 115, 111, 102, 116, 32, 40, 82, 41, 32, 72, 76, 83, 76, 32, 83, 104, 97, 100, 101, 114, 32, 67, 111, 109, 112, 105, 108, 101, 114, 32, 49, 48, 46, 48, 46, 49, 48, 48, 49, 49, 46, 49, 54, 51, 56, 52, 0, 73, 83, 71, 78, 104, 0, 0, 0, 3, 0, 0, 0, 8, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 3, 3, 0, 0, 89, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 15, 15, 0, 0, 95, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 3, 3, 0, 0, 80, 79, 83, 73, 84, 73, 79, 78, 0, 67, 79, 76, 79, 82, 0, 84, 69, 88, 67, 79, 79, 82, 68, 0, 79, 83, 71, 78, 108, 0, 0, 0, 3, 0, 0, 0, 8, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 92, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 15, 0, 0, 0, 98, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 3, 12, 0, 0, 83, 86, 95, 80, 79, 83, 73, 84, 73, 79, 78, 0, 67, 79, 76, 79, 82, 0, 84, 69, 88, 67, 79, 79, 82, 68, 0, 171, 83, 72, 68, 82, 0, 1, 0, 0, 64, 0, 1, 0, 64, 0, 0, 0, 89, 0, 0, 4, 70, 142, 32, 0, 0, 0, 0, 0, 4, 0, 0, 0, 95, 0, 0, 3, 50, 16, 16, 0, 0, 0, 0, 0, 95, 0, 0, 3, 242, 16, 16, 0, 1, 0, 0, 0, 95, 0, 0, 3, 50, 16, 16, 0, 2, 0, 0, 0, 103, 0, 0, 4, 242, 32, 16, 0, 0, 0, 0, 0, 1, 0, 0, 0, 101, 0, 0, 3, 242, 32, 16, 0, 1, 0, 0, 0, 101, 0, 0, 3, 50, 32, 16, 0, 2, 0, 0, 0, 104, 0, 0, 2, 1, 0, 0, 0, 56, 0, 0, 8, 242, 0, 16, 0, 0, 0, 0, 0, 86, 21, 16, 0, 0, 0, 0, 0, 70, 142, 32, 0, 0, 0, 0, 0, 1, 0, 0, 0, 50, 0, 0, 10, 242, 0, 16, 0, 0, 0, 0, 0, 70, 142, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 16, 16, 0, 0, 0, 0, 0, 70, 14, 16, 0, 0, 0, 0, 0, 0, 0, 0, 8, 242, 32, 16, 0, 0, 0, 0, 0, 70, 14, 16, 0, 0, 0, 0, 0, 70, 142, 32, 0, 0, 0, 0, 0, 3, 0, 0, 0, 54, 0, 0, 5, 242, 32, 16, 0, 1, 0, 0, 0, 70, 30, 16, 0, 1, 0, 0, 0, 54, 0, 0, 5, 50, 32, 16, 0, 2, 0, 0, 0, 70, 16, 16, 0, 2, 0, 0, 0, 62, 0, 0, 1, 83, 84, 65, 84, 116, 0, 0, 0, 6, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			hr = _device->CreateVertexShader(vs, sizeof(vs), &_imgui_vertex_shader);

			if (FAILED(hr))
			{
				return false;
			}

			// Create the input layout
			D3D10_INPUT_ELEMENT_DESC input_layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(ImDrawVert, pos), D3D10_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(ImDrawVert, uv), D3D10_INPUT_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(ImDrawVert, col), D3D10_INPUT_PER_VERTEX_DATA, 0 },
			};

			hr = _device->CreateInputLayout(input_layout, _countof(input_layout), vs, sizeof(vs), &_imgui_input_layout);

			if (FAILED(hr))
			{
				return false;
			}
		}

		// Create the pixel shader
		{
			const BYTE ps[] = { 68, 88, 66, 67, 244, 55, 63, 173, 76, 165, 188, 216, 93, 202, 217, 79, 224, 122, 206, 154, 1, 0, 0, 0, 160, 2, 0, 0, 5, 0, 0, 0, 52, 0, 0, 0, 224, 0, 0, 0, 84, 1, 0, 0, 136, 1, 0, 0, 36, 2, 0, 0, 82, 68, 69, 70, 164, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 28, 0, 0, 0, 0, 4, 255, 255, 0, 1, 0, 0, 110, 0, 0, 0, 92, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 101, 0, 0, 0, 2, 0, 0, 0, 5, 0, 0, 0, 4, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 1, 0, 0, 0, 12, 0, 0, 0, 115, 97, 109, 112, 108, 101, 114, 48, 0, 116, 101, 120, 116, 117, 114, 101, 48, 0, 77, 105, 99, 114, 111, 115, 111, 102, 116, 32, 40, 82, 41, 32, 72, 76, 83, 76, 32, 83, 104, 97, 100, 101, 114, 32, 67, 111, 109, 112, 105, 108, 101, 114, 32, 49, 48, 46, 48, 46, 49, 48, 48, 49, 49, 46, 49, 54, 51, 56, 52, 0, 171, 171, 73, 83, 71, 78, 108, 0, 0, 0, 3, 0, 0, 0, 8, 0, 0, 0, 80, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 92, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 15, 15, 0, 0, 98, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 3, 3, 0, 0, 83, 86, 95, 80, 79, 83, 73, 84, 73, 79, 78, 0, 67, 79, 76, 79, 82, 0, 84, 69, 88, 67, 79, 79, 82, 68, 0, 171, 79, 83, 71, 78, 44, 0, 0, 0, 1, 0, 0, 0, 8, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 15, 0, 0, 0, 83, 86, 95, 84, 97, 114, 103, 101, 116, 0, 171, 171, 83, 72, 68, 82, 148, 0, 0, 0, 64, 0, 0, 0, 37, 0, 0, 0, 90, 0, 0, 3, 0, 96, 16, 0, 0, 0, 0, 0, 88, 24, 0, 4, 0, 112, 16, 0, 0, 0, 0, 0, 85, 85, 0, 0, 98, 16, 0, 3, 242, 16, 16, 0, 1, 0, 0, 0, 98, 16, 0, 3, 50, 16, 16, 0, 2, 0, 0, 0, 101, 0, 0, 3, 242, 32, 16, 0, 0, 0, 0, 0, 104, 0, 0, 2, 1, 0, 0, 0, 69, 0, 0, 9, 242, 0, 16, 0, 0, 0, 0, 0, 70, 16, 16, 0, 2, 0, 0, 0, 70, 126, 16, 0, 0, 0, 0, 0, 0, 96, 16, 0, 0, 0, 0, 0, 56, 0, 0, 7, 242, 32, 16, 0, 0, 0, 0, 0, 70, 14, 16, 0, 0, 0, 0, 0, 70, 30, 16, 0, 1, 0, 0, 0, 62, 0, 0, 1, 83, 84, 65, 84, 116, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			hr = _device->CreatePixelShader(ps, sizeof(ps), &_imgui_pixel_shader);

			if (FAILED(hr))
			{
				return false;
			}
		}

		// Create the constant buffer
		{
			D3D10_BUFFER_DESC desc = { };
			desc.ByteWidth = 16 * sizeof(float);
			desc.Usage = D3D10_USAGE_DYNAMIC;
			desc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

			hr = _device->CreateBuffer(&desc, nullptr, &_imgui_constant_buffer);

			if (FAILED(hr))
			{
				return false;
			}
		}

		// Create the blending setup
		{
			D3D10_BLEND_DESC desc = { };
			desc.BlendEnable[0] = true;
			desc.SrcBlend = D3D10_BLEND_SRC_ALPHA;
			desc.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
			desc.BlendOp = D3D10_BLEND_OP_ADD;
			desc.SrcBlendAlpha = D3D10_BLEND_INV_SRC_ALPHA;
			desc.DestBlendAlpha = D3D10_BLEND_ZERO;
			desc.BlendOpAlpha = D3D10_BLEND_OP_ADD;
			desc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;

			hr = _device->CreateBlendState(&desc, &_imgui_blend_state);

			if (FAILED(hr))
			{
				return false;
			}
		}

		// Create the rasterizer state
		{
			D3D10_RASTERIZER_DESC desc = { };
			desc.FillMode = D3D10_FILL_SOLID;
			desc.CullMode = D3D10_CULL_NONE;
			desc.ScissorEnable = true;
			desc.DepthClipEnable = true;

			hr = _device->CreateRasterizerState(&desc, &_imgui_rasterizer_state);

			if (FAILED(hr))
			{
				return false;
			}
		}
		
		// Create texture sampler
		{
			D3D10_SAMPLER_DESC desc = { };
			desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = D3D10_TEXTURE_ADDRESS_WRAP;
			desc.AddressV = D3D10_TEXTURE_ADDRESS_WRAP;
			desc.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
			desc.ComparisonFunc = D3D10_COMPARISON_ALWAYS;

			hr = _device->CreateSamplerState(&desc, &_imgui_texture_sampler);

			if (FAILED(hr))
			{
				return false;
			}
		}

		return true;
	}
	bool d3d10_runtime::init_imgui_font_atlas()
	{
		int width, height;
		unsigned char *pixels;

		ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

		const D3D10_TEXTURE2D_DESC tex_desc = {
			static_cast<UINT>(width),
			static_cast<UINT>(height),
			1,
			1,
			DXGI_FORMAT_R8G8B8A8_UNORM,
			{ 1, 0 },
			D3D10_USAGE_DEFAULT,
			D3D10_BIND_SHADER_RESOURCE
		};
		const D3D10_SUBRESOURCE_DATA tex_data = {
			pixels,
			tex_desc.Width * 4
		};

		com_ptr<ID3D10Texture2D> font_atlas;
		com_ptr<ID3D10ShaderResourceView> font_atlas_view;

		HRESULT hr = _device->CreateTexture2D(&tex_desc, &tex_data, &font_atlas);

		if (FAILED(hr))
		{
			return false;
		}

		hr = _device->CreateShaderResourceView(font_atlas.get(), nullptr, &font_atlas_view);

		if (FAILED(hr))
		{
			return false;
		}

		const auto obj = new d3d10_texture();
		obj->width = width;
		obj->height = height;
		obj->levels = 1;
		obj->format = texture_format::rgba8;
		obj->texture = font_atlas;
		obj->srv[0] = font_atlas_view;

		_imgui_font_atlas.reset(obj);

		return true;
	}

	bool d3d10_runtime::on_init(const DXGI_SWAP_CHAIN_DESC &desc)
	{
		_width = desc.BufferDesc.Width;
		_height = desc.BufferDesc.Height;
		_backbuffer_format = desc.BufferDesc.Format;
		_is_multisampling_enabled = desc.SampleDesc.Count > 1;
		_input = input::register_window(desc.OutputWindow);

		if (!init_backbuffer_texture() ||
			!init_default_depth_stencil() ||
			!init_fx_resources() ||
			!init_imgui_resources() ||
			!init_imgui_font_atlas())
		{
			return false;
		}

		return runtime::on_init();
	}
	void d3d10_runtime::on_reset()
	{
		if (!_is_initialized)
		{
			return;
		}

		runtime::on_reset();

		// Destroy resources
		_backbuffer.reset();
		_backbuffer_resolved.reset();
		_backbuffer_texture.reset();
		_backbuffer_texture_srv[0].reset();
		_backbuffer_texture_srv[1].reset();
		_backbuffer_rtv[0].reset();
		_backbuffer_rtv[1].reset();
		_backbuffer_rtv[2].reset();

		_depthstencil.reset();
		_depthstencil_replacement.reset();
		_depthstencil_texture.reset();
		_depthstencil_texture_srv.reset();

		_default_depthstencil.reset();
		_copy_vertex_shader.reset();
		_copy_pixel_shader.reset();
		_copy_sampler.reset();

		_effect_rasterizer_state.reset();

		_imgui_vertex_buffer.reset();
		_imgui_index_buffer.reset();
		_imgui_vertex_shader.reset();
		_imgui_pixel_shader.reset();
		_imgui_input_layout.reset();
		_imgui_constant_buffer.reset();
		_imgui_texture_sampler.reset();
		_imgui_rasterizer_state.reset();
		_imgui_blend_state.reset();
		_imgui_vertex_buffer_size = 0;
		_imgui_index_buffer_size = 0;
	}
	void d3d10_runtime::on_reset_effect()
	{
		runtime::on_reset_effect();

		for (auto it : _effect_sampler_states)
		{
			it->Release();
		}

		_effect_sampler_states.clear();
		_effect_shader_resources.clear();

		_constant_buffer.reset();
		_constant_buffer_size = 0;
	}
	void d3d10_runtime::on_present()
	{
		if (!_is_initialized)
		{
			LOG(TRACE) << "Failed to present! Runtime is in a lost state.";
			return;
		}
		else if (_drawcalls == 0)
		{
			return;
		}

		detect_depth_source();

		// Capture device state
		_stateblock.capture();

		// Resolve back buffer
		if (_backbuffer_resolved != _backbuffer)
		{
			_device->ResolveSubresource(_backbuffer_resolved.get(), 0, _backbuffer.get(), 0, _backbuffer_format);
		}

		// Apply post processing
		on_apply_effect();

		// Reset render target
		const auto render_target = _backbuffer_rtv[0].get();
		_device->OMSetRenderTargets(1, &render_target, _default_depthstencil.get());

		const D3D10_VIEWPORT viewport = { 0, 0, _width, _height, 0.0f, 1.0f };
		_device->RSSetViewports(1, &viewport);

		// Apply presenting
		runtime::on_present();

		// Copy to back buffer
		if (_backbuffer_resolved != _backbuffer)
		{
			const auto rtv = _backbuffer_rtv[2].get();
			_device->OMSetRenderTargets(1, &rtv, nullptr);
			_device->CopyResource(_backbuffer_texture.get(), _backbuffer_resolved.get());

			_device->VSSetShader(_copy_vertex_shader.get());
			_device->PSSetShader(_copy_pixel_shader.get());
			const auto sst = _copy_sampler.get();
			_device->PSSetSamplers(0, 1, &sst);
			const auto srv = _backbuffer_texture_srv[make_format_srgb(_backbuffer_format) == _backbuffer_format].get();
			_device->PSSetShaderResources(0, 1, &srv);
			_device->Draw(3, 0);
		}

		// Apply previous device state
		_stateblock.apply_and_release();
	}
	void d3d10_runtime::on_draw_call(UINT vertices)
	{
		runtime::on_draw_call(vertices);

		com_ptr<ID3D10DepthStencilView> current_depthstencil;

		_device->OMGetRenderTargets(0, nullptr, &current_depthstencil);

		if (current_depthstencil == nullptr || current_depthstencil == _default_depthstencil)
		{
			return;
		}
		if (current_depthstencil == _depthstencil_replacement)
		{
			current_depthstencil = _depthstencil;
		}

		const auto it = _depth_source_table.find(current_depthstencil.get());

		if (it != _depth_source_table.end())
		{
			it->second.drawcall_count = static_cast<float>(_drawcalls);
			it->second.vertices_count += vertices;
		}
	}
	void d3d10_runtime::on_apply_effect()
	{
		if (_techniques.empty())
		{
			return;
		}

		// Setup real back buffer
		const auto render_target = _backbuffer_rtv[0].get();
		_device->OMSetRenderTargets(1, &render_target, nullptr);

		// Setup vertex input
		const uintptr_t null = 0;
		_device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		_device->IASetInputLayout(nullptr);
		_device->IASetVertexBuffers(0, 1, reinterpret_cast<ID3D10Buffer *const *>(&null), reinterpret_cast<const UINT *>(&null), reinterpret_cast<const UINT *>(&null));

		_device->RSSetState(_effect_rasterizer_state.get());

		// Disable unused pipeline stages
		_device->GSSetShader(nullptr);

		// Setup samplers
		_device->VSSetSamplers(0, static_cast<UINT>(_effect_sampler_states.size()), _effect_sampler_states.data());
		_device->PSSetSamplers(0, static_cast<UINT>(_effect_sampler_states.size()), _effect_sampler_states.data());

		// Setup shader constants
		const auto constant_buffer = _constant_buffer.get();
		_device->VSSetConstantBuffers(0, 1, &constant_buffer);
		_device->PSSetConstantBuffers(0, 1, &constant_buffer);

		// Apply post processing
		runtime::on_apply_effect();
	}
	void d3d10_runtime::on_apply_effect_technique(const technique &technique)
	{
		runtime::on_apply_effect_technique(technique);

		bool is_default_depthstencil_cleared = false;

		// Update shader constants
		if (_constant_buffer != nullptr)
		{
			void *data = nullptr;

			const HRESULT hr = _constant_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &data);

			if (SUCCEEDED(hr))
			{
				CopyMemory(data, get_uniform_value_storage().data(), _constant_buffer_size);

				_constant_buffer->Unmap();
			}
			else
			{
				LOG(TRACE) << "Failed to map constant buffer! HRESULT is '" << std::hex << hr << std::dec << "'!";
			}
		}

		for (const auto &pass_ptr : technique.passes)
		{
			const auto &pass = *static_cast<const d3d10_pass *>(pass_ptr.get());

			// Setup states
			_device->VSSetShader(pass.vertex_shader.get());
			_device->PSSetShader(pass.pixel_shader.get());

			const float blendfactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			_device->OMSetBlendState(pass.blend_state.get(), blendfactor, D3D10_DEFAULT_SAMPLE_MASK);
			_device->OMSetDepthStencilState(pass.depth_stencil_state.get(), pass.stencil_reference);

			// Save back buffer of previous pass
			_device->CopyResource(_backbuffer_texture.get(), _backbuffer_resolved.get());

			// Setup shader resources
			_device->VSSetShaderResources(0, static_cast<UINT>(pass.shader_resources.size()), pass.shader_resources.data());
			_device->PSSetShaderResources(0, static_cast<UINT>(pass.shader_resources.size()), pass.shader_resources.data());

			// Setup render targets
			if (pass.viewport.Width == _width && pass.viewport.Height == _height)
			{
				_device->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, pass.render_targets, _default_depthstencil.get());

				if (!is_default_depthstencil_cleared)
				{
					is_default_depthstencil_cleared = true;

					_device->ClearDepthStencilView(_default_depthstencil.get(), D3D10_CLEAR_DEPTH | D3D10_CLEAR_STENCIL, 1.0f, 0);
				}
			}
			else
			{
				_device->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, pass.render_targets, nullptr);
			}

			_device->RSSetViewports(1, &pass.viewport);

			if (pass.clear_render_targets)
			{
				for (const auto target : pass.render_targets)
				{
					if (target != nullptr)
					{
						const float color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						_device->ClearRenderTargetView(target, color);
					}
				}
			}

			// Draw triangle
			_device->Draw(3, 0);

			runtime::on_draw_call(3);

			// Reset render targets
			_device->OMSetRenderTargets(0, nullptr, nullptr);

			// Reset shader resources
			ID3D10ShaderResourceView *null[D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = { nullptr };
			_device->VSSetShaderResources(0, static_cast<UINT>(pass.shader_resources.size()), null);
			_device->PSSetShaderResources(0, static_cast<UINT>(pass.shader_resources.size()), null);

			// Update shader resources
			for (const auto resource : pass.render_target_resources)
			{
				if (resource == nullptr)
				{
					continue;
				}

				D3D10_SHADER_RESOURCE_VIEW_DESC resource_desc;
				resource->GetDesc(&resource_desc);

				if (resource_desc.Texture2D.MipLevels > 1)
				{
					_device->GenerateMips(resource);
				}
			}
		}
	}

	void d3d10_runtime::on_set_depthstencil_view(ID3D10DepthStencilView *&depthstencil)
	{
		if (_depth_source_table.find(depthstencil) == _depth_source_table.end())
		{
			D3D10_TEXTURE2D_DESC texture_desc;
			com_ptr<ID3D10Resource> resource;
			com_ptr<ID3D10Texture2D> texture;

			depthstencil->GetResource(&resource);

			if (FAILED(resource->QueryInterface(&texture)))
			{
				return;
			}

			texture->GetDesc(&texture_desc);

			// Early depth-stencil rejection
			if (texture_desc.Width != _width || texture_desc.Height != _height || texture_desc.SampleDesc.Count > 1)
			{
				return;
			}

			LOG(TRACE) << "Adding depth-stencil " << depthstencil << " (Width: " << texture_desc.Width << ", Height: " << texture_desc.Height << ", Format: " << texture_desc.Format << ") to list of possible depth candidates ...";

			depthstencil->AddRef();

			// Begin tracking new depth-stencil
			const depth_source_info info = { texture_desc.Width, texture_desc.Height };
			_depth_source_table.emplace(depthstencil, info);
		}

		if (_depthstencil_replacement != nullptr && depthstencil == _depthstencil)
		{
			depthstencil = _depthstencil_replacement.get();
		}
	}
	void d3d10_runtime::on_get_depthstencil_view(ID3D10DepthStencilView *&depthstencil)
	{
		if (_depthstencil_replacement != nullptr && depthstencil == _depthstencil_replacement)
		{
			depthstencil->Release();

			depthstencil = _depthstencil.get();

			depthstencil->AddRef();
		}
	}
	void d3d10_runtime::on_clear_depthstencil_view(ID3D10DepthStencilView *&depthstencil)
	{
		if (_depthstencil_replacement != nullptr && depthstencil == _depthstencil)
		{
			depthstencil = _depthstencil_replacement.get();
		}
	}
	void d3d10_runtime::on_copy_resource(ID3D10Resource *&dest, ID3D10Resource *&source)
	{
		if (_depthstencil_replacement != nullptr)
		{
			com_ptr<ID3D10Resource> resource;
			_depthstencil->GetResource(&resource);

			if (dest == resource)
			{
				dest = _depthstencil_texture.get();
			}
			if (source == resource)
			{
				source = _depthstencil_texture.get();
			}
		}
	}

	void d3d10_runtime::screenshot(unsigned char *buffer) const
	{
		if (_backbuffer_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
			_backbuffer_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
			_backbuffer_format != DXGI_FORMAT_B8G8R8A8_UNORM &&
			_backbuffer_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
		{
			LOG(WARNING) << "Screenshots are not supported for back buffer format " << _backbuffer_format << ".";
			return;
		}

		D3D10_TEXTURE2D_DESC texture_desc = { };
		texture_desc.Width = _width;
		texture_desc.Height = _height;
		texture_desc.Format = _backbuffer_format;
		texture_desc.MipLevels = 1;
		texture_desc.ArraySize = 1;
		texture_desc.SampleDesc.Count = 1;
		texture_desc.Usage = D3D10_USAGE_STAGING;
		texture_desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;

		com_ptr<ID3D10Texture2D> texture_staging;

		HRESULT hr = _device->CreateTexture2D(&texture_desc, nullptr, &texture_staging);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create staging texture for screenshot capture! HRESULT is '" << std::hex << hr << std::dec << "'.";
			return;
		}

		_device->CopyResource(texture_staging.get(), _backbuffer_resolved.get());
				
		D3D10_MAPPED_TEXTURE2D mapped;
		hr = texture_staging->Map(0, D3D10_MAP_READ, 0, &mapped);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to map staging texture with screenshot capture! HRESULT is '" << std::hex << hr << std::dec << "'.";
			return;
		}

		auto mapped_data = static_cast<BYTE *>(mapped.pData);
		const UINT pitch = texture_desc.Width * 4;

		for (UINT y = 0; y < texture_desc.Height; y++)
		{
			CopyMemory(buffer, mapped_data, std::min(pitch, static_cast<UINT>(mapped.RowPitch)));
			
			for (UINT x = 0; x < pitch; x += 4)
			{
				buffer[x + 3] = 0xFF;

				if (_backbuffer_format == DXGI_FORMAT_B8G8R8A8_UNORM || _backbuffer_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
				{
					std::swap(buffer[x + 0], buffer[x + 2]);
				}
			}
								
			buffer += pitch;
			mapped_data += mapped.RowPitch;
		}

		texture_staging->Unmap(0);
	}
	bool d3d10_runtime::update_effect(const fx::syntax_tree &ast, const std::vector<std::string> &pragmas, std::string &errors)
	{
		bool skip_optimization = false;

		for (const auto &pragma : pragmas)
		{
			fx::lexer lexer(pragma);

			const auto prefix_token = lexer.lex();

			if (prefix_token.literal_as_string != "reshade")
			{
				continue;
			}

			const auto command_token = lexer.lex();

			if (command_token.literal_as_string == "skipoptimization" || command_token.literal_as_string == "nooptimization")
			{
				skip_optimization = true;
			}
		}

		return d3d10_fx_compiler(this, ast, errors, skip_optimization).run();
	}
	bool d3d10_runtime::update_texture(texture &texture, const unsigned char *data, size_t size)
	{
		const auto texture_impl = dynamic_cast<d3d10_texture *>(&texture);

		assert(texture_impl != nullptr);
		assert(data != nullptr && size != 0);

		if (texture.type != texture_type::image)
		{
			return false;
		}

		assert(texture.height != 0);

		_device->UpdateSubresource(texture_impl->texture.get(), 0, nullptr, data, static_cast<UINT>(size / texture.height), static_cast<UINT>(size));

		if (texture.levels > 1)
		{
			_device->GenerateMips(texture_impl->srv[0].get());
		}

		return true;
	}
	void d3d10_runtime::update_texture_datatype(d3d10_texture &texture, texture_type source, const com_ptr<ID3D10ShaderResourceView> &srv, const com_ptr<ID3D10ShaderResourceView> &srv_srgb)
	{
		const auto srv_srgb_ptr = srv_srgb == nullptr ? srv.get() : srv_srgb.get();

		texture.type = source;

		if (srv == texture.srv[0] && srv_srgb_ptr == texture.srv[1])
		{
			return;
		}

		texture.rtv[0].reset();
		texture.rtv[1].reset();
		texture.srv[0].reset();
		texture.srv[1].reset();
		texture.texture.reset();

		if (srv != nullptr)
		{
			texture.srv[0] = srv;
			texture.srv[1] = srv_srgb_ptr;

			texture.srv[0]->GetResource(reinterpret_cast<ID3D10Resource **>(&texture.texture));

			D3D10_TEXTURE2D_DESC desc;
			texture.texture->GetDesc(&desc);

			texture.width = desc.Width;
			texture.height = desc.Height;
			texture.format = texture_format::unknown;
			texture.levels = desc.MipLevels;
		}
		else
		{
			texture.width = texture.height = texture.levels = 0;
			texture.format = texture_format::unknown;
		}

		// Update techniques shader resource views
		for (const auto &technique : _techniques)
		{
			for (const auto &pass_ptr : technique.passes)
			{
				auto &pass = *static_cast<d3d10_pass *>(pass_ptr.get());

				pass.shader_resources[texture.shader_register] = texture.srv[0].get();
				pass.shader_resources[texture.shader_register + 1] = texture.srv[1].get();
			}
		}
	}

	void d3d10_runtime::render_draw_lists(ImDrawData *draw_data)
	{
		// Create and grow vertex/index buffers if needed
		if (_imgui_vertex_buffer == nullptr || _imgui_vertex_buffer_size < draw_data->TotalVtxCount)
		{
			_imgui_vertex_buffer.reset();
			_imgui_vertex_buffer_size = draw_data->TotalVtxCount + 5000;

			D3D10_BUFFER_DESC desc = { };
			desc.Usage = D3D10_USAGE_DYNAMIC;
			desc.ByteWidth = _imgui_vertex_buffer_size * sizeof(ImDrawVert);
			desc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
			desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;

			if (FAILED(_device->CreateBuffer(&desc, nullptr, &_imgui_vertex_buffer)))
			{
				return;
			}
		}
		if (_imgui_index_buffer == nullptr || _imgui_index_buffer_size < draw_data->TotalIdxCount)
		{
			_imgui_index_buffer.reset();
			_imgui_index_buffer_size = draw_data->TotalIdxCount + 10000;

			D3D10_BUFFER_DESC desc = { };
			desc.Usage = D3D10_USAGE_DYNAMIC;
			desc.ByteWidth = _imgui_index_buffer_size * sizeof(ImDrawIdx);
			desc.BindFlags = D3D10_BIND_INDEX_BUFFER;
			desc.CPUAccessFlags = D3D10_CPU_ACCESS_WRITE;

			if (FAILED(_device->CreateBuffer(&desc, nullptr, &_imgui_index_buffer)))
			{
				return;
			}
		}

		ImDrawVert *vtx_dst;
		ImDrawIdx *idx_dst;

		if (FAILED(_imgui_vertex_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, reinterpret_cast<void **>(&vtx_dst))) ||
			FAILED(_imgui_index_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, reinterpret_cast<void **>(&idx_dst))))
		{
			return;
		}

		for (int n = 0; n < draw_data->CmdListsCount; n++)
		{
			const auto cmd_list = draw_data->CmdLists[n];

			CopyMemory(vtx_dst, &cmd_list->VtxBuffer[0], cmd_list->VtxBuffer.size() * sizeof(ImDrawVert));
			CopyMemory(idx_dst, &cmd_list->IdxBuffer[0], cmd_list->IdxBuffer.size() * sizeof(ImDrawIdx));

			vtx_dst += cmd_list->VtxBuffer.size();
			idx_dst += cmd_list->IdxBuffer.size();
		}

		_imgui_vertex_buffer->Unmap();
		_imgui_index_buffer->Unmap();

		// Setup orthographic projection matrix
		void *constant_buffer_data;

		if (FAILED(_imgui_constant_buffer->Map(D3D10_MAP_WRITE_DISCARD, 0, &constant_buffer_data)))
		{
			return;
		}

		const float ortho_projection[16] =
		{
			2.0f / _width, 0.0f, 0.0f, 0.0f,
			0.0f, -2.0f / _height, 0.0f, 0.0f,
			0.0f, 0.0f, 0.5f, 0.0f,
			-1.0f, 1.0f, 0.5f, 1.0f
		};

		CopyMemory(constant_buffer_data, ortho_projection, sizeof(ortho_projection));

		_imgui_constant_buffer->Unmap();

		// Setup render state
		const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
		_device->OMSetBlendState(_imgui_blend_state.get(), blend_factor, 0xffffffff);
		_device->RSSetState(_imgui_rasterizer_state.get());

		UINT stride = sizeof(ImDrawVert), offset = 0;
		ID3D10Buffer *vertex_buffers[1] = { _imgui_vertex_buffer.get() }, *constant_buffers[1] = { _imgui_constant_buffer.get() };
		ID3D10SamplerState *samplers[1] = { _imgui_texture_sampler.get() };
		_device->IASetInputLayout(_imgui_input_layout.get());
		_device->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
		_device->IASetIndexBuffer(_imgui_index_buffer.get(), sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
		_device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		_device->VSSetShader(_imgui_vertex_shader.get());
		_device->VSSetConstantBuffers(0, 1, constant_buffers);
		_device->PSSetShader(_imgui_pixel_shader.get());
		_device->PSSetSamplers(0, 1, samplers);

		// Render command lists
		UINT vtx_offset = 0, idx_offset = 0;

		for (int n = 0; n < draw_data->CmdListsCount; n++)
		{
			const auto cmd_list = draw_data->CmdLists[n];

			for (auto cmd = cmd_list->CmdBuffer.begin(); cmd != cmd_list->CmdBuffer.end(); idx_offset += cmd->ElemCount, cmd++)
			{
				if (cmd->UserCallback != nullptr)
				{
					cmd->UserCallback(cmd_list, cmd);
				}
				else
				{
					const D3D10_RECT scissor_rect = {
						static_cast<LONG>(cmd->ClipRect.x),
						static_cast<LONG>(cmd->ClipRect.y),
						static_cast<LONG>(cmd->ClipRect.z),
						static_cast<LONG>(cmd->ClipRect.w)
					};

					ID3D10ShaderResourceView *const texture_view = static_cast<const d3d10_texture *>(cmd->TextureId)->srv[0].get();
					_device->PSSetShaderResources(0, 1, &texture_view);
					_device->RSSetScissorRects(1, &scissor_rect);

					_device->DrawIndexed(cmd->ElemCount, idx_offset, vtx_offset);
				}
			}

			vtx_offset += cmd_list->VtxBuffer.size();
		}
	}

	void d3d10_runtime::detect_depth_source()
	{
		static int cooldown = 0, traffic = 0;

		if (cooldown-- > 0)
		{
			traffic += g_network_traffic > 0;
			return;
		}
		else
		{
			cooldown = 30;

			if (traffic > 10)
			{
				traffic = 0;
				create_depthstencil_replacement(nullptr);
				return;
			}
			else
			{
				traffic = 0;
			}
		}

		if (_is_multisampling_enabled || _depth_source_table.empty())
		{
			return;
		}

		depth_source_info best_info = { 0 };
		ID3D10DepthStencilView *best_match = nullptr;

		for (auto it = _depth_source_table.begin(); it != _depth_source_table.end();)
		{
			const auto depthstencil = it->first;
			auto &depthstencil_info = it->second;

			if ((depthstencil->AddRef(), depthstencil->Release()) == 1)
			{
				LOG(TRACE) << "Removing depth-stencil " << depthstencil << " from list of possible depth candidates ...";

				depthstencil->Release();

				it = _depth_source_table.erase(it);
				continue;
			}
			else
			{
				++it;
			}

			if (depthstencil_info.drawcall_count == 0)
			{
				continue;
			}

			if ((depthstencil_info.vertices_count * (1.2f - depthstencil_info.drawcall_count / _drawcalls)) >= (best_info.vertices_count * (1.2f - best_info.drawcall_count / _drawcalls)))
			{
				best_match = depthstencil;
				best_info = depthstencil_info;
			}

			depthstencil_info.drawcall_count = depthstencil_info.vertices_count = 0;
		}

		if (best_match != nullptr && _depthstencil != best_match)
		{
			LOG(TRACE) << "Switched depth source to depth-stencil " << best_match << ".";

			create_depthstencil_replacement(best_match);
		}
	}
	bool d3d10_runtime::create_depthstencil_replacement(ID3D10DepthStencilView *depthstencil)
	{
		_depthstencil.reset();
		_depthstencil_replacement.reset();
		_depthstencil_texture.reset();
		_depthstencil_texture_srv.reset();

		if (depthstencil != nullptr)
		{
			_depthstencil = depthstencil;

			_depthstencil->GetResource(reinterpret_cast<ID3D10Resource **>(&_depthstencil_texture));

			D3D10_TEXTURE2D_DESC texdesc;
			_depthstencil_texture->GetDesc(&texdesc);

			HRESULT hr = S_OK;

			if ((texdesc.BindFlags & D3D10_BIND_SHADER_RESOURCE) == 0)
			{
				_depthstencil_texture.reset();

				switch (texdesc.Format)
				{
					case DXGI_FORMAT_R16_TYPELESS:
					case DXGI_FORMAT_D16_UNORM:
						texdesc.Format = DXGI_FORMAT_R16_TYPELESS;
						break;
					case DXGI_FORMAT_R32_TYPELESS:
					case DXGI_FORMAT_D32_FLOAT:
						texdesc.Format = DXGI_FORMAT_R32_TYPELESS;
						break;
					default:
					case DXGI_FORMAT_R24G8_TYPELESS:
					case DXGI_FORMAT_D24_UNORM_S8_UINT:
						texdesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
						break;
					case DXGI_FORMAT_R32G8X24_TYPELESS:
					case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
						texdesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
						break;
				}

				texdesc.BindFlags = D3D10_BIND_DEPTH_STENCIL | D3D10_BIND_SHADER_RESOURCE;

				hr = _device->CreateTexture2D(&texdesc, nullptr, &_depthstencil_texture);

				if (SUCCEEDED(hr))
				{
					D3D10_DEPTH_STENCIL_VIEW_DESC dsvdesc = { };
					dsvdesc.ViewDimension = D3D10_DSV_DIMENSION_TEXTURE2D;

					switch (texdesc.Format)
					{
						case DXGI_FORMAT_R16_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D16_UNORM;
							break;
						case DXGI_FORMAT_R32_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D32_FLOAT;
							break;
						case DXGI_FORMAT_R24G8_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
							break;
						case DXGI_FORMAT_R32G8X24_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
							break;
					}

					hr = _device->CreateDepthStencilView(_depthstencil_texture.get(), &dsvdesc, &_depthstencil_replacement);
				}
			}
			else
			{
				_depthstencil_replacement = _depthstencil;
			}

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create depth-stencil replacement texture! HRESULT is '" << std::hex << hr << std::dec << "'.";

				return false;
			}

			D3D10_SHADER_RESOURCE_VIEW_DESC srvdesc = { };
			srvdesc.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
			srvdesc.Texture2D.MipLevels = 1;

			switch (texdesc.Format)
			{
				case DXGI_FORMAT_R16_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R16_FLOAT;
					break;
				case DXGI_FORMAT_R32_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R32_FLOAT;
					break;
				case DXGI_FORMAT_R24G8_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
					break;
				case DXGI_FORMAT_R32G8X24_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
					break;
			}

			hr = _device->CreateShaderResourceView(_depthstencil_texture.get(), &srvdesc, &_depthstencil_texture_srv);

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create depth-stencil replacement resource view! HRESULT is '" << std::hex << hr << std::dec << "'.";

				return false;
			}

			if (_depthstencil != _depthstencil_replacement)
			{
				// Update auto depth-stencil
				com_ptr<ID3D10DepthStencilView> current_depthstencil;
				ID3D10RenderTargetView *targets[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };

				_device->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &current_depthstencil);

				if (current_depthstencil == _depthstencil)
				{
					_device->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, _depthstencil_replacement.get());
				}

				for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
				{
					if (targets[i] != nullptr)
					{
						targets[i]->Release();
					}
				}
			}
		}

		// Update effect textures
		for (const auto &texture : _textures)
		{
			if (texture->type == texture_type::depthbuffer)
			{
				update_texture_datatype(static_cast<d3d10_texture &>(*texture), texture_type::depthbuffer, _depthstencil_texture_srv, nullptr);
			}
		}

		return true;
	}
}