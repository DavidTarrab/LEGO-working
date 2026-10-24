#include <Windows.h>
#include <stdint.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#include "MinHook/include/MinHook.h"
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <fstream>
#include <queue>
#include <wrl/client.h>
#pragma comment( lib, "winmm" )
#include "E:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.1\\include\\nvml.h"
#include "E:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.1\\include\\cuda_runtime.h"
#pragma comment(lib, "E:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.1\\lib\\x64\\nvml.lib")
#pragma comment(lib, "E:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.1\\lib\\x64\\cudart.lib")

using namespace std;
using namespace Microsoft::WRL;


//--------------------------------------------------------------------------------------------------------------------//


#define WIDTH 30

LARGE_INTEGER injectStamp;
LARGE_INTEGER counterStamp;

char stateText[50] = "123";
char rendertimeText[50] = "123";

bool prend = false;
bool InitOnce = true;
bool record_cmd = false;

double fps = 0;
double rendertime = 0;

int curr_fence = 1;
int framenum = 0;
int cmdnum = 0;

ofstream debuglog;
ofstream presentlog;
FILE* rendertimelog;

string debugLogName = "_debug.txt";
string presentLogName = "_present.txt";
string renderTimeLogName = "_render.csv";
string LogFilesPath = "E:\\Program Files\\RenderHook\\";

ID3D12Fence* fence;
ID3D12Device* d3dDevice = NULL;
ComPtr<ID3D12CommandQueue>			pICMDQueue;
ComPtr<ID3D12RootSignature>			pIRootSignature;
ComPtr<ID3D12PipelineState>			pIPipelineState;
ComPtr<ID3D12CommandAllocator>		pICMDAlloc;
ComPtr<ID3D12GraphicsCommandList>	pICMDList;
ComPtr<ID3DBlob> pIBlobVertexShader;
ComPtr<ID3DBlob> pIBlobPixelShader;

struct Queryparam
{
	HANDLE event;
	int fnum;
	int cnum;
	int type;
	int isEnding;
	LARGE_INTEGER entertime;
	ID3D12Fence* fence1;
};

queue <Queryparam*> testq;
Queryparam* BeginParam, * EndParam;

typedef long(WINAPI* Present12) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
Present12 oPresent12 = NULL;

typedef void(WINAPI* ExecuteCommandLists12)(ID3D12CommandQueue* This, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
ExecuteCommandLists12 oExecuteCommandLists = NULL;

LARGE_INTEGER BeginStamp, EndStamp;


//--------------------------------------------------------------------------------------------------------------------//


namespace dx12
{
	struct Status
	{
		enum Enum
		{
			UnknownError = -1,
			NotSupportedError = -2,
			ModuleNotFoundError = -3,
			Success = 0,
		};
	};

	struct RenderType
	{
		enum Enum
		{
			None,
			D3D12,
		};
	};

	Status::Enum init(RenderType::Enum renderType);

	RenderType::Enum getRenderType();

#if _M_X64
	uint64_t* getMethodsTable();
#elif defined _M_IX86
	uint32_t* getMethodsTable();
#endif
}


static dx12::RenderType::Enum gRenderType = dx12::RenderType::None;

#if _M_X64
static uint64_t* gMethodsTable = NULL;
#elif defined _M_IX86
static uint32_t* gMethodsTable = NULL;
#endif

dx12::Status::Enum dx12::init(RenderType::Enum _renderType)
{
	if (_renderType != RenderType::None)
	{
		if (_renderType == RenderType::D3D12)
		{
			WNDCLASSEX windowClass;
			windowClass.cbSize = sizeof(WNDCLASSEX);
			windowClass.style = CS_HREDRAW | CS_VREDRAW;
			windowClass.lpfnWndProc = DefWindowProc;
			windowClass.cbClsExtra = 0;
			windowClass.cbWndExtra = 0;
			windowClass.hInstance = GetModuleHandle(NULL);
			windowClass.hIcon = NULL;
			windowClass.hCursor = NULL;
			windowClass.hbrBackground = NULL;
			windowClass.lpszMenuName = NULL;
			windowClass.lpszClassName = TEXT("dx12");
			windowClass.hIconSm = NULL;

			::RegisterClassEx(&windowClass);

			HWND window = ::CreateWindow(windowClass.lpszClassName, TEXT("DirectX Window"), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, windowClass.hInstance, NULL);


			if (_renderType == RenderType::D3D12)
			{
				HMODULE libDXGI;
				HMODULE libD3D12;
				if ((libDXGI = ::GetModuleHandle(TEXT("dxgi.dll"))) == NULL || (libD3D12 = ::GetModuleHandle(TEXT("d3d12.dll"))) == NULL)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::ModuleNotFoundError;
				}

				void* CreateDXGIFactory;
				if ((CreateDXGIFactory = ::GetProcAddress(libDXGI, "CreateDXGIFactory")) == NULL)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				IDXGIFactory* factory;
				if (((long(__stdcall*)(const IID&, void**))(CreateDXGIFactory))(__uuidof(IDXGIFactory), (void**)&factory) < 0)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				IDXGIAdapter* adapter;
				if (factory->EnumAdapters(0, &adapter) == DXGI_ERROR_NOT_FOUND)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				void* D3D12CreateDevice;
				if ((D3D12CreateDevice = ::GetProcAddress(libD3D12, "D3D12CreateDevice")) == NULL)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				ID3D12Device* device;
				if (((long(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**))(D3D12CreateDevice))(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device) < 0) //why is D3D_FEATURE_LEVEL_12_0 wrong?
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				D3D12_COMMAND_QUEUE_DESC queueDesc;
				queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				queueDesc.Priority = 0;
				queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
				queueDesc.NodeMask = 0;

				ID3D12CommandQueue* commandQueue;
				if (device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&commandQueue) < 0)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				ID3D12CommandAllocator* commandAllocator;
				if (device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&commandAllocator) < 0)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				ID3D12GraphicsCommandList* commandList;
				if (device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, NULL, __uuidof(ID3D12GraphicsCommandList), (void**)&commandList) < 0)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				DXGI_RATIONAL refreshRate;
				refreshRate.Numerator = 60;
				refreshRate.Denominator = 1;

				DXGI_MODE_DESC bufferDesc;
				bufferDesc.Width = 100;
				bufferDesc.Height = 100;
				bufferDesc.RefreshRate = refreshRate;
				bufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				bufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
				bufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

				DXGI_SAMPLE_DESC sampleDesc;
				sampleDesc.Count = 1;
				sampleDesc.Quality = 0;

				DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
				swapChainDesc.BufferDesc = bufferDesc;
				swapChainDesc.SampleDesc = sampleDesc;
				swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
				swapChainDesc.BufferCount = 2;
				swapChainDesc.OutputWindow = window;
				swapChainDesc.Windowed = 1;
				swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
				swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

				IDXGISwapChain* swapChain;
				if (factory->CreateSwapChain(commandQueue, &swapChainDesc, &swapChain) < 0)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}

				ID3D12Fence* fence;
				if (device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
					IID_PPV_ARGS(&fence)) < 0)
				{
					::DestroyWindow(window);
					::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
					return Status::UnknownError;
				}
#if _M_X64
				gMethodsTable = (uint64_t*)::calloc(1150, sizeof(uint64_t));       // 分配虚函数表
				memcpy(gMethodsTable, *(uint64_t**)device, 44 * sizeof(uint64_t));      // 虚函数表的位置是对象所在内存的开始，*(uint64_t**)device可以获得虚函数表的首地址
				memcpy(gMethodsTable + 44, *(uint64_t**)commandQueue, 19 * sizeof(uint64_t));
				memcpy(gMethodsTable + 44 + 19, *(uint64_t**)commandAllocator, 9 * sizeof(uint64_t));
				memcpy(gMethodsTable + 44 + 19 + 9, *(uint64_t**)commandList, 60 * sizeof(uint64_t));
				memcpy(gMethodsTable + 44 + 19 + 9 + 60, *(uint64_t**)swapChain, 18 * sizeof(uint64_t));
#elif defined _M_IX86
				gMethodsTable = (uint32_t*)::calloc(161, sizeof(uint32_t));
				memcpy(gMethodsTable, *(uint32_t**)device, 44 * sizeof(uint32_t));
				memcpy(gMethodsTable + 44, *(uint32_t**)commandQueue, 19 * sizeof(uint32_t));
				memcpy(gMethodsTable + 44 + 19, *(uint32_t**)commandAllocator, 9 * sizeof(uint32_t));
				memcpy(gMethodsTable + 44 + 19 + 9, *(uint32_t**)commandList, 60 * sizeof(uint32_t));
				memcpy(gMethodsTable + 44 + 19 + 9 + 60, *(uint32_t**)swapChain, 18 * sizeof(uint32_t));
#endif

				device->Release();
				device = NULL;

				commandQueue->Release();
				commandQueue = NULL;

				commandAllocator->Release();
				commandAllocator = NULL;

				commandList->Release();
				commandList = NULL;

				swapChain->Release();
				swapChain = NULL;

				fence->Release();
				fence = NULL;

				::DestroyWindow(window);
				::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);

				gRenderType = RenderType::D3D12;

				return Status::Success;
			}

			return Status::NotSupportedError;
		}

	}

	return Status::Success;
}


dx12::RenderType::Enum dx12::getRenderType()
{
	return gRenderType;
}

#if defined _M_X64
uint64_t* dx12::getMethodsTable()
{
	return gMethodsTable;
}
#elif defined _M_IX86
uint32_t* dx12::getMethodsTable()
{
	return gMethodsTable;
}
#endif


//--------------------------------------------------------------------------------------------------------------------//


LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	HDC hdc;
	PAINTSTRUCT ps;
	RECT rect;
	int wmId, wmEvent;
	static int counter = 0;
	float realtime = 0;
	switch (message)
	{
	case WM_CREATE:
		return 0;

	case WM_PAINT:
		hdc = BeginPaint(hwnd, &ps);
		TextOutA(hdc, 10, 20, (LPCSTR)stateText, strlen(stateText));
		TextOutA(hdc, 10, WIDTH + 20, (LPCSTR)rendertimeText, strlen(rendertimeText));
		EndPaint(hwnd, &ps);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_TIMER:
		QueryPerformanceCounter(&counterStamp);
		realtime = (counterStamp.QuadPart - injectStamp.QuadPart) / 1e7;
		sprintf_s(stateText, "time  %.3f s", realtime);
		counter++;
		RECT rect2;
		GetClientRect(hwnd, &rect2);
		UpdateWindow(hwnd);
		RedrawWindow(hwnd, &rect2, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
		return 0;
	}
	return DefWindowProc(hwnd, message, wParam, lParam);

}

DWORD WINAPI Create_window(LPVOID lpParam)
{
	static TCHAR szAppName[] = TEXT("debug_w");
	HWND hwnd;
	MSG msg;
	WNDCLASS wndclass;
	wndclass.style = CS_HREDRAW | CS_VREDRAW;
	wndclass.cbClsExtra = 0;
	wndclass.cbWndExtra = 0;
	wndclass.hInstance = GetModuleHandle(NULL);
	wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
	wndclass.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wndclass.lpszMenuName = NULL;
	wndclass.lpfnWndProc = WndProc;
	wndclass.lpszClassName = szAppName;
	if (!RegisterClass(&wndclass))
	{
		MessageBox(NULL, TEXT("This program requires Windows NT!"), szAppName, MB_ICONERROR);
		return 0;
	};
	hwnd = CreateWindow(szAppName,      // window class name
		TEXT("game"),   // window caption
		WS_OVERLAPPEDWINDOW, // window style
		50,// initial x position
		50,// initial y position
		500,// initial x size
		200,// initial y size
		NULL, // parent window handle
		NULL, // window menu handle
		GetModuleHandle(NULL), // program instance handle
		NULL);

	SetTimer(hwnd, 1, 100, NULL);
	ShowWindow(hwnd, SW_SHOWNORMAL);
	UpdateWindow(hwnd);

	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return msg.wParam;
}


//--------------------------------------------------------------------------------------------------------------------//


byte GetGPURate_byte() {
	nvmlReturn_t result;
	unsigned int device_count, i;
	// First initialize NVML library
	result = nvmlInit();

	result = nvmlDeviceGetCount(&device_count);
	if (NVML_SUCCESS != result)
	{
		return 0;
	}

	for (i = 0; i < device_count; i++)
	{
		nvmlDevice_t device;
		char name[NVML_DEVICE_NAME_BUFFER_SIZE];
		nvmlPciInfo_t pci;
		result = nvmlDeviceGetHandleByIndex(i, &device);
		if (NVML_SUCCESS != result) {
			return 0;
		}
		result = nvmlDeviceGetName(device, name, NVML_DEVICE_NAME_BUFFER_SIZE);
		if (NVML_SUCCESS != result) {
			return 0;
		}
		nvmlUtilization_t utilization;
		result = nvmlDeviceGetUtilizationRates(device, &utilization);
		if (NVML_SUCCESS != result)
		{
			return 0;
		}
		return ((byte)utilization.gpu);
	}
	return 0;
}

std::string GetCurrentProcessName() {
	char processName[MAX_PATH] = {0};

	// 使用 GetModuleFileName 获取当前进程主模块（即 .exe 文件）的路径
	if (GetModuleFileNameA(NULL, processName, MAX_PATH)) {
		std::string fullPath(processName);

		// 提取文件名部分



		
		size_t lastSlash = fullPath.find_last_of("\\/");
		std::string fileName = fullPath.substr(lastSlash + 1);  // 提取出文件名部分

		// 查找并去掉 ".exe" 后缀
		size_t extPos = fileName.find(".exe");
		if (extPos != std::string::npos) {
			fileName = fileName.substr(0, fileName.size() - 4);  // 去掉 ".exe"
		}

		return fileName;
	}

	return "<unknown>";
}


//--------------------------------------------------------------------------------------------------------------------//


long WINAPI hkPresent12(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	if (InitOnce)
	{
		InitOnce = false;
		//get device 
		if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3dDevice)))
		{
			pSwapChain->GetDevice(__uuidof(d3dDevice), (void**)&d3dDevice);
			d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
				IID_PPV_ARGS(&fence));
			D3D12_ROOT_SIGNATURE_DESC stRootSignatureDesc =
			{
				0
				, nullptr
				, 0
				, nullptr
				, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
			};

			ComPtr<ID3DBlob> pISignatureBlob;
			ComPtr<ID3DBlob> pIErrorBlob;

			D3D12SerializeRootSignature(
				&stRootSignatureDesc
				, D3D_ROOT_SIGNATURE_VERSION_1
				, &pISignatureBlob
				, &pIErrorBlob);

			d3dDevice->CreateRootSignature(0
				, pISignatureBlob->GetBufferPointer()
				, pISignatureBlob->GetBufferSize()
				, IID_PPV_ARGS(&pIRootSignature));
			D3D12_INPUT_ELEMENT_DESC stInputElementDescs[] =
			{
				{
					"POSITION"
					, 0
					, DXGI_FORMAT_R32G32B32A32_FLOAT
					, 0
					, 0
					, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
					, 0
				},
				{
					"COLOR"
					, 0
					, DXGI_FORMAT_R32G32B32A32_FLOAT
					, 0
					, 16
					, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
					, 0
				}
			};
			UINT nCompileFlags = 0;
			D3DCompileFromFile(L"E:\\Program Files\\RenderHook\\RenderHookDLL\\shaders.hlsl"
				, nullptr
				, nullptr
				, "VSMain"
				, "vs_5_0"
				, nCompileFlags
				, 0
				, &pIBlobVertexShader
				, nullptr);
			D3DCompileFromFile(L"E:\\Program Files\\RenderHook\\RenderHookDLL\\shaders.hlsl"
				, nullptr
				, nullptr
				, "PSMain"
				, "ps_5_0"
				, nCompileFlags
				, 0
				, &pIBlobPixelShader
				, nullptr);
			D3D12_GRAPHICS_PIPELINE_STATE_DESC stPSODesc = {};

			stPSODesc.InputLayout = { stInputElementDescs, _countof(stInputElementDescs) };
			stPSODesc.pRootSignature = pIRootSignature.Get();
			stPSODesc.VS.pShaderBytecode = pIBlobVertexShader->GetBufferPointer();
			stPSODesc.VS.BytecodeLength = pIBlobVertexShader->GetBufferSize();
			stPSODesc.PS.pShaderBytecode = pIBlobPixelShader->GetBufferPointer();
			stPSODesc.PS.BytecodeLength = pIBlobPixelShader->GetBufferSize();

			stPSODesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			stPSODesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;

			stPSODesc.BlendState.AlphaToCoverageEnable = FALSE;
			stPSODesc.BlendState.IndependentBlendEnable = FALSE;
			stPSODesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

			stPSODesc.DepthStencilState.DepthEnable = FALSE;
			stPSODesc.DepthStencilState.StencilEnable = FALSE;

			stPSODesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

			stPSODesc.NumRenderTargets = 1;
			stPSODesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

			stPSODesc.SampleMask = UINT_MAX;
			stPSODesc.SampleDesc.Count = 1;

			d3dDevice->CreateGraphicsPipelineState(&stPSODesc, IID_PPV_ARGS(&pIPipelineState));
			d3dDevice->CreateCommandAllocator(
				D3D12_COMMAND_LIST_TYPE_DIRECT
				, IID_PPV_ARGS(&pICMDAlloc));

			d3dDevice->CreateCommandList(
				0
				, D3D12_COMMAND_LIST_TYPE_DIRECT
				, pICMDAlloc.Get()
				, pIPipelineState.Get()
				, IID_PPV_ARGS(&pICMDList));
		}
	}
	
	//presentlog << "frame " << framenum << " pre begin " << PresentStamp.QuadPart  << endl;
	
	if (record_cmd)
	{
		record_cmd = false;
		//debuglog << "[d3d12]====== enter queue 1 " << HookBeginStamp.QuadPart << " type " << EndParam->type << " frame " << EndParam->fnum << " cmd num " << EndParam->cnum << endl;
		testq.push(EndParam);
		EndParam = new Queryparam;
	}
	
	framenum++;
	auto ret = oPresent12(pSwapChain, SyncInterval, Flags);

	prend = true;
	return ret;
}

void WINAPI hkExecuteCommandLists12(
	ID3D12CommandQueue* This,
	UINT              NumCommandLists,
	ID3D12CommandList* const* ppCommandLists)
{
	LARGE_INTEGER HookBeginStamp;
	QueryPerformanceCounter(&HookBeginStamp);
	
	//debuglog << "[d3d12]====== ExecuteCommandLists hooked " << HookBeginStamp.QuadPart << " cmdqueue " << This << " type " << This->GetDesc().Type << " cmd " << ppCommandLists << " frame " << framenum << " cmd num " << cmdnum << endl;
	if (This->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT && prend)
	{
		prend = false;
		pICMDList->Reset(pICMDAlloc.Get(), pIPipelineState.Get());

		pICMDList->SetGraphicsRootSignature(pIRootSignature.Get());
		pICMDList->SetPipelineState(pIPipelineState.Get());
		pICMDList->DrawInstanced(0, 0, 0, 0);
		pICMDList->Close();
		ID3D12CommandList* ppCommandLists[] = { pICMDList.Get() };
		
		oExecuteCommandLists(This, _countof(ppCommandLists), ppCommandLists);
		
		uint64_t set = curr_fence;
		This->Signal(fence, set);
		++curr_fence;
		HANDLE event = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
		//debuglog << "set fence " << curr_fence - 1 << endl;
		if (fence->GetCompletedValue() < set)
		{
			//debuglog << "set event " << curr_fence - 1 << endl;
			fence->SetEventOnCompletion(set, event);
			QueryPerformanceCounter(&HookBeginStamp);
			BeginParam = new Queryparam;
			BeginParam->event = event;
			BeginParam->fnum = framenum;
			BeginParam->cnum = cmdnum;
			BeginParam->type = This->GetDesc().Type;
			BeginParam->entertime = HookBeginStamp;
			BeginParam->isEnding = 0;
			testq.push(BeginParam);
			//debuglog << "[d3d12]====== enter queue 0 " << HookBeginStamp.QuadPart << " cmdqueue " << This << " type " << This->GetDesc().Type << " cmd " << ppCommandLists << " frame " << framenum << " cmd num " << cmdnum << endl;
		}
	}
	
	oExecuteCommandLists(This, NumCommandLists, ppCommandLists);

	if (This->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
	{
		HANDLE event = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
		uint64_t set = curr_fence;
		This->Signal(fence, set);
		++curr_fence;
		if (fence->GetCompletedValue() < set)
		{
			fence->SetEventOnCompletion(set, event);
			QueryPerformanceCounter(&HookBeginStamp);
			EndParam->event = event;
			EndParam->fnum = framenum;
			EndParam->cnum = cmdnum;
			EndParam->type = This->GetDesc().Type;
			EndParam->entertime = HookBeginStamp;
			EndParam->isEnding = 1;
			record_cmd = true;
			cmdnum++;
		}
	}
}


//--------------------------------------------------------------------------------------------------------------------//


DWORD WINAPI queryrenderstate(LPVOID lpParam)
{
	LARGE_INTEGER BeginStamp, EndStamp;
	QueryPerformanceCounter(&BeginStamp);
	QueryPerformanceCounter(&EndStamp);
	while (true)
	{
		if (testq.empty())
		{
			Sleep(3);
		}
		else {
			Queryparam* a = testq.front();
			HANDLE waitevent = a->event;
			WaitForSingleObject(waitevent, 60);
			LARGE_INTEGER Stamp;
			QueryPerformanceCounter(&Stamp);
			if (a->isEnding)
			{
				EndStamp = Stamp;
				//fps = 6e8 / (double)(presentqueue.back().QuadPart - presentqueue.front().QuadPart);
				rendertime = (double)(EndStamp.QuadPart - BeginStamp.QuadPart) / 1e4;
				sprintf_s(rendertimeText, "rendertime %.3lf", rendertime);
				QueryPerformanceCounter(&counterStamp);
				double realtime = (counterStamp.QuadPart - injectStamp.QuadPart) / 1e7;
				fprintf(rendertimelog, "%lf,%d,%lf,%d\n", realtime, framenum, rendertime, GetGPURate_byte());
			}
			else
			{
				BeginStamp = Stamp;
			}
			delete a;
			testq.pop();
		}
	}
}

int dx12Thread()
{
	if (dx12::init(dx12::RenderType::D3D12) == dx12::Status::Success)
	{
		MH_Initialize();
		
		MH_CreateHook((LPVOID)dx12::getMethodsTable()[140], hkPresent12, (LPVOID*)&oPresent12);
		MH_CreateHook((LPVOID)dx12::getMethodsTable()[54], hkExecuteCommandLists12, (LPVOID*)&oExecuteCommandLists);
		MH_EnableHook((LPVOID)dx12::getMethodsTable()[140]);
		MH_EnableHook((LPVOID)dx12::getMethodsTable()[54]);
	}
	else
	{
		debuglog << "init d3d12 failed !" << endl;
	}
	
	return 0;
}


//====================================================================================================================//


BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD fdwReason, LPVOID)
{
	string gameName = GetCurrentProcessName();
	
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hInstance);
		BeginParam = new Queryparam;
		EndParam = new Queryparam;
		QueryPerformanceCounter(&injectStamp);
		QueryPerformanceCounter(&counterStamp);
		debuglog.open(LogFilesPath + gameName + debugLogName);
		presentlog.open(LogFilesPath + gameName + presentLogName);
		rendertimelog = fopen((LogFilesPath + gameName + renderTimeLogName).c_str(), "w+");
		fprintf(rendertimelog, "realtime,framenum,rendertime(ms),GPUUtil(%%)\n");
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Create_window, NULL, 0, NULL);
		CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)dx12Thread, NULL, 0, NULL);
		CreateThread(NULL, NULL, queryrenderstate, NULL, 0, NULL);
		break;
	case DLL_PROCESS_DETACH: // A process unloads the DLL.
		MessageBoxA(0, "DLL removed", "", 3);
		if (MH_DisableHook((LPVOID)dx12::getMethodsTable()[140]) != MH_OK) { return 1; }
		if (MH_DisableHook((LPVOID)dx12::getMethodsTable()[54]) != MH_OK) { return 1; }
		if (MH_Uninitialize() != MH_OK) { return 1; }
		fclose(rendertimelog);
		break;
	}

	return TRUE;
}