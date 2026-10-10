using System.Runtime.InteropServices;

namespace VisionFlow.Desktop.Capture;

/// <summary>
/// MediaFoundation 属性存储接口。
///
/// <para>
/// 方法顺序必须与 Windows SDK 中 <c>mfobjects.h</c> 的 vtable 严格一致：
/// IUnknown 的 3 个方法（QueryInterface/AddRef/Release）之后，
/// 依次是 GetItem / GetItemType / CompareItem / Compare /
/// GetUINT32 / GetUINT64 / GetDouble / GetGUID / GetStringLength / GetString /
/// GetAllocatedString / GetBlobSize / GetBlob / GetAllocatedBlob / GetUnknown /
/// SetItem / DeleteItem / DeleteAllItems / SetUINT32 / SetUINT64 /
/// SetDouble / SetGUID / SetString / SetBlob / SetUnknown / LockStore /
/// UnlockStore / GetCount / GetItemByIndex / CopyAllItems。
/// </para>
/// <para>
/// C# 的 <c>ComImport</c> + <c>InterfaceIsIUnknown</c> 要求**按 vtable 顺序**
/// 声明所有前面的方法（哪怕是占位），否则索引错位会调到错误的函数上，
/// 表现为随机崩溃或内存损坏 —— 这是手写 COM 最典型的坑。
/// </para>
/// </summary>
[ComImport]
[Guid("2CD2D921-C447-44A7-A13C-4ADABFC247E3")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFAttributes
{
    // ── IUnknown 的 3 个方法由 CLR 自动处理，此处不声明 ──

    // ① GetItem / GetItemType / CompareItem 共 3 个
    [PreserveSig]
    int GetItem([In] ref Guid guidKey, IntPtr pValue);

    [PreserveSig]
    int GetItemType([In] ref Guid guidKey, out uint pType);

    [PreserveSig]
    int CompareItem([In] ref Guid guidKey, IntPtr pValue, out int pbResult);

    [PreserveSig]
    int Compare(IMFAttributes pTheirs, int matchType, out int pbResult);

    // ② 数值读取
    [PreserveSig]
    int GetUINT32([In] ref Guid guidKey, out uint punValue);

    [PreserveSig]
    int GetUINT64([In] ref Guid guidKey, out ulong punValue);

    [PreserveSig]
    int GetDouble([In] ref Guid guidKey, out double pfValue);

    [PreserveSig]
    int GetGUID([In] ref Guid guidKey, out Guid pguidValue);

    // ③ 字符串读取（长度 → 取 → 释放）
    [PreserveSig]
    int GetStringLength([In] ref Guid guidKey, out uint pcchLength);

    [PreserveSig]
    int GetString([In] ref Guid guidKey, [Out] char[] pwszValue, uint cchBufSize, IntPtr pcchLength);

    [PreserveSig]
    int GetAllocatedString([In] ref Guid guidKey, out IntPtr ppwszValue, out uint pcchLength);

    [PreserveSig]
    int GetBlobSize([In] ref Guid guidKey, out uint pcbBlobSize);

    [PreserveSig]
    int GetBlob([In] ref Guid guidKey, [Out] byte[] pBuf, uint cbBufSize, IntPtr pcbBlobSize);

    [PreserveSig]
    int GetAllocatedBlob([In] ref Guid guidKey, out IntPtr ppBuf, out uint pcbSize);

    [PreserveSig]
    int GetUnknown([In] ref Guid guidKey, [In] ref Guid riid, out IntPtr ppv);

    [PreserveSig]
    int SetItem([In] ref Guid guidKey, IntPtr pValue);

    [PreserveSig]
    int DeleteItem([In] ref Guid guidKey);

    [PreserveSig]
    int DeleteAllItems();

    [PreserveSig]
    int SetUINT32([In] ref Guid guidKey, uint unValue);

    [PreserveSig]
    int SetUINT64([In] ref Guid guidKey, ulong unValue);

    [PreserveSig]
    int SetDouble([In] ref Guid guidKey, double fValue);

    [PreserveSig]
    int SetGUID([In] ref Guid guidKey, [In] ref Guid guidValue);

    [PreserveSig]
    int SetString([In] ref Guid guidKey, [MarshalAs(UnmanagedType.LPWStr)] string wszValue);

    [PreserveSig]
    int SetBlob([In] ref Guid guidKey, [In] byte[] pBuf, uint cbBufSize);

    [PreserveSig]
    int SetUnknown([In] ref Guid guidKey, [In] IntPtr pUnknown);

    [PreserveSig]
    int LockStore();

    [PreserveSig]
    int UnlockStore();

    [PreserveSig]
    int GetCount(out uint pcItems);

    [PreserveSig]
    int GetItemByIndex(uint unIndex, out Guid pguidKey, IntPtr pValue);

    [PreserveSig]
    int CopyAllItems(IMFAttributes pDest);
}

/// <summary>
/// 媒体类型。继承 <see cref="IMFAttributes"/>，另有
/// GetMajorType / IsCompressedFormat / IsEqual / GetRepresentation /
/// FreeRepresentation 五个方法。
/// </summary>
[ComImport]
[Guid("44AE0FA8-EA31-4109-8D2E-4CAE4997C555")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFMediaType
{
    // ── IMFAttributes 部分（vtable 前缀，必须完整复刻）──
    [PreserveSig] int GetItem([In] ref Guid guidKey, IntPtr pValue);
    [PreserveSig] int GetItemType([In] ref Guid guidKey, out uint pType);
    [PreserveSig] int CompareItem([In] ref Guid guidKey, IntPtr pValue, out int pbResult);
    [PreserveSig] int Compare(IMFAttributes pTheirs, int matchType, out int pbResult);
    [PreserveSig] int GetUINT32([In] ref Guid guidKey, out uint punValue);
    [PreserveSig] int GetUINT64([In] ref Guid guidKey, out ulong punValue);
    [PreserveSig] int GetDouble([In] ref Guid guidKey, out double pfValue);
    [PreserveSig] int GetGUID([In] ref Guid guidKey, out Guid pguidValue);
    [PreserveSig] int GetStringLength([In] ref Guid guidKey, out uint pcchLength);
    [PreserveSig] int GetString([In] ref Guid guidKey, [Out] char[] pwszValue, uint cchBufSize, IntPtr pcchLength);
    [PreserveSig] int GetAllocatedString([In] ref Guid guidKey, out IntPtr ppwszValue, out uint pcchLength);
    [PreserveSig] int GetBlobSize([In] ref Guid guidKey, out uint pcbBlobSize);
    [PreserveSig] int GetBlob([In] ref Guid guidKey, [Out] byte[] pBuf, uint cbBufSize, IntPtr pcbBlobSize);
    [PreserveSig] int GetAllocatedBlob([In] ref Guid guidKey, out IntPtr ppBuf, out uint pcbSize);
    [PreserveSig] int GetUnknown([In] ref Guid guidKey, [In] ref Guid riid, out IntPtr ppv);
    [PreserveSig] int SetItem([In] ref Guid guidKey, IntPtr pValue);
    [PreserveSig] int DeleteItem([In] ref Guid guidKey);
    [PreserveSig] int DeleteAllItems();
    [PreserveSig] int SetUINT32([In] ref Guid guidKey, uint unValue);
    [PreserveSig] int SetUINT64([In] ref Guid guidKey, ulong unValue);
    [PreserveSig] int SetDouble([In] ref Guid guidKey, double fValue);
    [PreserveSig] int SetGUID([In] ref Guid guidKey, [In] ref Guid guidValue);
    [PreserveSig] int SetString([In] ref Guid guidKey, [MarshalAs(UnmanagedType.LPWStr)] string wszValue);
    [PreserveSig] int SetBlob([In] ref Guid guidKey, [In] byte[] pBuf, uint cbBufSize);
    [PreserveSig] int SetUnknown([In] ref Guid guidKey, [In] IntPtr pUnknown);
    [PreserveSig] int LockStore();
    [PreserveSig] int UnlockStore();
    [PreserveSig] int GetCount(out uint pcItems);
    [PreserveSig] int GetItemByIndex(uint unIndex, out Guid pguidKey, IntPtr pValue);
    [PreserveSig] int CopyAllItems(IMFAttributes pDest);

    // ── IMFMediaType 自有 ──
    [PreserveSig] int GetMajorType(out Guid pguidMajorType);

    [PreserveSig] int IsCompressedFormat(out int pfCompressed);

    [PreserveSig] int IsEqual(IMFMediaType pIMediaType, out uint pdwFlags);

    [PreserveSig] int GetRepresentation(Guid guidRepresentation, out IntPtr ppvRepresentation);

    [PreserveSig] int FreeRepresentation(Guid guidRepresentation, IntPtr pvRepresentation);
}

/// <summary>
/// 设备激活对象。除了 IMFAttributes 前缀，另有
/// ActivateObject / ShutdownObject / DetachObject 三个方法。
/// </summary>
[ComImport]
[Guid("7FEE9E9A-4A89-47A6-899C-B6A53A70FB67")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFActivate
{
    // ── IMFAttributes 部分 ──
    [PreserveSig] int GetItem([In] ref Guid guidKey, IntPtr pValue);
    [PreserveSig] int GetItemType([In] ref Guid guidKey, out uint pType);
    [PreserveSig] int CompareItem([In] ref Guid guidKey, IntPtr pValue, out int pbResult);
    [PreserveSig] int Compare(IMFAttributes pTheirs, int matchType, out int pbResult);
    [PreserveSig] int GetUINT32([In] ref Guid guidKey, out uint punValue);
    [PreserveSig] int GetUINT64([In] ref Guid guidKey, out ulong punValue);
    [PreserveSig] int GetDouble([In] ref Guid guidKey, out double pfValue);
    [PreserveSig] int GetGUID([In] ref Guid guidKey, out Guid pguidValue);
    [PreserveSig] int GetStringLength([In] ref Guid guidKey, out uint pcchLength);
    [PreserveSig] int GetString([In] ref Guid guidKey, [Out] char[] pwszValue, uint cchBufSize, IntPtr pcchLength);
    [PreserveSig] int GetAllocatedString([In] ref Guid guidKey, out IntPtr ppwszValue, out uint pcchLength);
    [PreserveSig] int GetBlobSize([In] ref Guid guidKey, out uint pcbBlobSize);
    [PreserveSig] int GetBlob([In] ref Guid guidKey, [Out] byte[] pBuf, uint cbBufSize, IntPtr pcbBlobSize);
    [PreserveSig] int GetAllocatedBlob([In] ref Guid guidKey, out IntPtr ppBuf, out uint pcbSize);
    [PreserveSig] int GetUnknown([In] ref Guid guidKey, [In] ref Guid riid, out IntPtr ppv);
    [PreserveSig] int SetItem([In] ref Guid guidKey, IntPtr pValue);
    [PreserveSig] int DeleteItem([In] ref Guid guidKey);
    [PreserveSig] int DeleteAllItems();
    [PreserveSig] int SetUINT32([In] ref Guid guidKey, uint unValue);
    [PreserveSig] int SetUINT64([In] ref Guid guidKey, ulong unValue);
    [PreserveSig] int SetDouble([In] ref Guid guidKey, double fValue);
    [PreserveSig] int SetGUID([In] ref Guid guidKey, [In] ref Guid guidValue);
    [PreserveSig] int SetString([In] ref Guid guidKey, [MarshalAs(UnmanagedType.LPWStr)] string wszValue);
    [PreserveSig] int SetBlob([In] ref Guid guidKey, [In] byte[] pBuf, uint cbBufSize);
    [PreserveSig] int SetUnknown([In] ref Guid guidKey, [In] IntPtr pUnknown);
    [PreserveSig] int LockStore();
    [PreserveSig] int UnlockStore();
    [PreserveSig] int GetCount(out uint pcItems);
    [PreserveSig] int GetItemByIndex(uint unIndex, out Guid pguidKey, IntPtr pValue);
    [PreserveSig] int CopyAllItems(IMFAttributes pDest);

    // ── IMFActivate 自有 ──
    [PreserveSig]
    int ActivateObject([In] ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);

    [PreserveSig]
    int ShutdownObject();

    [PreserveSig]
    int DetachObject();
}

/// <summary>媒体源。此处只用到 Shutdown，其余方法按 vtable 顺序占位。</summary>
[ComImport]
[Guid("279A808D-AEC7-40C8-9C6B-A6B492C78A66")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFMediaSource
{
    // IMFMediaEventGenerator
    [PreserveSig] int GetEvent(uint dwFlags, out IntPtr ppEvent);
    [PreserveSig] int BeginGetEvent(IntPtr pCallback, IntPtr punkState);
    [PreserveSig] int EndGetEvent(IntPtr pResult, out IntPtr ppEvent);
    [PreserveSig] int QueueEvent(uint met, [In] ref Guid guidExtendedType, int hrStatus, IntPtr pvValue);

    // IMFMediaSource
    [PreserveSig] int GetCharacteristics(out uint pdwCharacteristics);
    [PreserveSig] int CreatePresentationDescriptor(out IntPtr ppPresentationDescriptor);
    [PreserveSig] int Start(IntPtr pPresentationDescriptor, IntPtr pguidTimeFormat, IntPtr pvarStartPosition);
    [PreserveSig] int Stop();
    [PreserveSig] int Pause();
    [PreserveSig] int Shutdown();
}

/// <summary>
/// 媒体采样。我们只用到 <c>ConvertToContiguousBuffer</c>，
/// 前面的属性方法按 vtable 顺序占位（IMFSample 继承 IMFAttributes）。
/// </summary>
[ComImport]
[Guid("C40A00F2-B93A-4D80-AE8C-5A1C634F58E4")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFSample
{
    // ── IMFAttributes 部分 ──
    [PreserveSig] int GetItem([In] ref Guid guidKey, IntPtr pValue);
    [PreserveSig] int GetItemType([In] ref Guid guidKey, out uint pType);
    [PreserveSig] int CompareItem([In] ref Guid guidKey, IntPtr pValue, out int pbResult);
    [PreserveSig] int Compare(IMFAttributes pTheirs, int matchType, out int pbResult);
    [PreserveSig] int GetUINT32([In] ref Guid guidKey, out uint punValue);
    [PreserveSig] int GetUINT64([In] ref Guid guidKey, out ulong punValue);
    [PreserveSig] int GetDouble([In] ref Guid guidKey, out double pfValue);
    [PreserveSig] int GetGUID([In] ref Guid guidKey, out Guid pguidValue);
    [PreserveSig] int GetStringLength([In] ref Guid guidKey, out uint pcchLength);
    [PreserveSig] int GetString([In] ref Guid guidKey, [Out] char[] pwszValue, uint cchBufSize, IntPtr pcchLength);
    [PreserveSig] int GetAllocatedString([In] ref Guid guidKey, out IntPtr ppwszValue, out uint pcchLength);
    [PreserveSig] int GetBlobSize([In] ref Guid guidKey, out uint pcbBlobSize);
    [PreserveSig] int GetBlob([In] ref Guid guidKey, [Out] byte[] pBuf, uint cbBufSize, IntPtr pcbBlobSize);
    [PreserveSig] int GetAllocatedBlob([In] ref Guid guidKey, out IntPtr ppBuf, out uint pcbSize);
    [PreserveSig] int GetUnknown([In] ref Guid guidKey, [In] ref Guid riid, out IntPtr ppv);
    [PreserveSig] int SetItem([In] ref Guid guidKey, IntPtr pValue);
    [PreserveSig] int DeleteItem([In] ref Guid guidKey);
    [PreserveSig] int DeleteAllItems();
    [PreserveSig] int SetUINT32([In] ref Guid guidKey, uint unValue);
    [PreserveSig] int SetUINT64([In] ref Guid guidKey, ulong unValue);
    [PreserveSig] int SetDouble([In] ref Guid guidKey, double fValue);
    [PreserveSig] int SetGUID([In] ref Guid guidKey, [In] ref Guid guidValue);
    [PreserveSig] int SetString([In] ref Guid guidKey, [MarshalAs(UnmanagedType.LPWStr)] string wszValue);
    [PreserveSig] int SetBlob([In] ref Guid guidKey, [In] byte[] pBuf, uint cbBufSize);
    [PreserveSig] int SetUnknown([In] ref Guid guidKey, [In] IntPtr pUnknown);
    [PreserveSig] int LockStore();
    [PreserveSig] int UnlockStore();
    [PreserveSig] int GetCount(out uint pcItems);
    [PreserveSig] int GetItemByIndex(uint unIndex, out Guid pguidKey, IntPtr pValue);
    [PreserveSig] int CopyAllItems(IMFAttributes pDest);

    // ── IMFSample 自有 ──
    [PreserveSig] int GetSampleFlags(out uint pdwSampleFlags);
    [PreserveSig] int SetSampleFlags(uint dwSampleFlags);
    [PreserveSig] int GetSampleTime(out long phnsSampleTime);
    [PreserveSig] int SetSampleTime(long hnsSampleTime);
    [PreserveSig] int GetSampleDuration(out long phnsSampleDuration);
    [PreserveSig] int SetSampleDuration(long hnsSampleDuration);
    [PreserveSig] int GetBufferCount(out uint pdwBufferCount);
    [PreserveSig] int GetBufferByIndex(uint dwIndex, out IMFMediaBuffer ppBuffer);
    [PreserveSig] int ConvertToContiguousBuffer(out IMFMediaBuffer ppBuffer);
    [PreserveSig] int AddBuffer(IMFMediaBuffer pBuffer);
    [PreserveSig] int RemoveBufferByIndex(uint dwIndex);
    [PreserveSig] int RemoveAllBuffers();
    [PreserveSig] int GetTotalLength(out uint pcbTotalLength);
    [PreserveSig] int CopyToBuffer(IMFMediaBuffer pBuffer);
}

/// <summary>媒体缓冲。用到 <c>Lock</c>/<c>Unlock</c>/<c>GetCurrentLength</c>。</summary>
[ComImport]
[Guid("045FA593-8799-42B8-BC8D-8968C6453507")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFMediaBuffer
{
    [PreserveSig]
    int Lock(out IntPtr ppbBuffer, out uint pcbMaxLength, out uint pcbCurrentLength);

    [PreserveSig]
    int Unlock();

    [PreserveSig]
    int GetCurrentLength(out uint pcbCurrentLength);

    [PreserveSig]
    int SetCurrentLength(uint cbCurrentLength);

    [PreserveSig]
    int GetMaxLength(out uint pcbMaxLength);
}

/// <summary>
/// 源读取器。用它同步读帧（<c>MF_SOURCE_READERF_*</c> 标志判断流结束/格式变化）。
/// </summary>
[ComImport]
[Guid("70AE66F2-C809-4E4F-8915-BDCB406B7993")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IMFSourceReader
{
    [PreserveSig]
    int GetStreamSelection(uint dwStreamIndex, out int pfSelected);

    [PreserveSig]
    int SetStreamSelection(uint dwStreamIndex, int fSelected);

    [PreserveSig]
    int GetNativeMediaType(uint dwStreamIndex, uint dwMediaTypeIndex, out IMFMediaType ppMediaType);

    [PreserveSig]
    int GetCurrentMediaType(uint dwStreamIndex, out IMFMediaType ppMediaType);

    [PreserveSig]
    int SetCurrentMediaType(uint dwStreamIndex, IntPtr pdwReserved, IMFMediaType pMediaType);

    [PreserveSig]
    int SetCurrentPosition([In] ref Guid guidTimeFormat, [In] ref long varPosition);

    [PreserveSig]
    int ReadSample(
        uint dwStreamIndex,
        uint dwControlFlags,
        out uint pdwActualStreamIndex,
        out uint pdwStreamFlags,
        out long pllTimestamp,
        out IMFSample? ppSample);

    [PreserveSig]
    int Flush(uint dwStreamIndex);

    [PreserveSig]
    int GetServiceForStream(uint dwStreamIndex, [In] ref Guid guidService, [In] ref Guid riid, out IntPtr ppvObject);

    [PreserveSig]
    int GetPresentationAttribute(uint dwStreamIndex, [In] ref Guid guidAttribute, IntPtr pvarAttribute);
}
