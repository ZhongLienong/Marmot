use std::ffi::CStr;
use std::os::raw::c_void;
use std::ptr;

use image::GenericImageView;

#[repr(C)]
struct ArrayArgument {
    data: *mut c_void,
    length: i32,
}

#[repr(C)]
struct FFIArray {
    data: *mut c_void,
    length: i32,
}

unsafe fn write_i64(ret: *mut c_void, value: i64) {
    ptr::write_unaligned(ret as *mut i64, value);
}

unsafe fn write_ptr(ret: *mut c_void, value: *mut c_void) {
    write_i64(ret, value as i64);
}

unsafe fn read_i64(args: *mut *mut c_void, index: usize) -> i64 {
    let raw = args.add(index).read() as u64;
    raw as i64
}

unsafe fn read_text(args: *mut *mut c_void, index: usize) -> Option<String> {
    let ptr = args.add(index).read() as *const i8;
    if ptr.is_null() {
        return None;
    }
    CStr::from_ptr(ptr).to_str().ok().map(|s| s.to_string())
}

unsafe fn read_byte_array(args: *mut *mut c_void, index: usize) -> Option<Vec<u8>> {
    let array_ptr = args.add(index).read() as *const ArrayArgument;
    if array_ptr.is_null() {
        return None;
    }
    let array = &*array_ptr;
    if array.length <= 0 || array.data.is_null() {
        return Some(Vec::new());
    }

    let count = array.length as usize;
    let data_ptr = array.data as *const u64;
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let raw = ptr::read_unaligned(data_ptr.add(i));
        out.push((raw & 0xFF) as u8);
    }
    Some(out)
}

unsafe fn alloc_array_from_u64(values: &[u64]) -> *mut FFIArray {
    if values.is_empty() {
        return ptr::null_mut();
    }

    let data_bytes = match values.len().checked_mul(std::mem::size_of::<u64>()) {
        Some(size) => size,
        None => return ptr::null_mut(),
    };

    let data_ptr = libc::malloc(data_bytes) as *mut u64;
    if data_ptr.is_null() {
        return ptr::null_mut();
    }

    for (i, value) in values.iter().enumerate() {
        ptr::write_unaligned(data_ptr.add(i), *value);
    }

    let array_ptr = libc::malloc(std::mem::size_of::<FFIArray>()) as *mut FFIArray;
    if array_ptr.is_null() {
        libc::free(data_ptr as *mut c_void);
        return ptr::null_mut();
    }

    (*array_ptr).data = data_ptr as *mut c_void;
    (*array_ptr).length = values.len() as i32;
    array_ptr
}

unsafe fn alloc_array_from_bytes(values: &[u8]) -> *mut FFIArray {
    if values.is_empty() {
        return ptr::null_mut();
    }

    let data_bytes = match values.len().checked_mul(std::mem::size_of::<u64>()) {
        Some(size) => size,
        None => return ptr::null_mut(),
    };

    let data_ptr = libc::malloc(data_bytes) as *mut u64;
    if data_ptr.is_null() {
        return ptr::null_mut();
    }

    for (i, value) in values.iter().enumerate() {
        ptr::write_unaligned(data_ptr.add(i), *value as u64);
    }

    let array_ptr = libc::malloc(std::mem::size_of::<FFIArray>()) as *mut FFIArray;
    if array_ptr.is_null() {
        libc::free(data_ptr as *mut c_void);
        return ptr::null_mut();
    }

    (*array_ptr).data = data_ptr as *mut c_void;
    (*array_ptr).length = values.len() as i32;
    array_ptr
}

#[no_mangle]
pub extern "C" fn marmot_image_read_info(args: *mut *mut c_void, ret: *mut c_void) {
    let path = unsafe { read_text(args, 0) };
    let Some(path) = path else {
        unsafe {
            write_ptr(ret, ptr::null_mut());
        }
        return;
    };

    let result = match image::open(&path) {
        Ok(img) => {
            let (width, height) = img.dimensions();
            let values = [width as u64, height as u64];
            unsafe { alloc_array_from_u64(&values) as *mut c_void }
        }
        Err(_) => ptr::null_mut(),
    };

    unsafe { write_ptr(ret, result) };
}

#[no_mangle]
pub extern "C" fn marmot_image_read_rgba(args: *mut *mut c_void, ret: *mut c_void) {
    let path = unsafe { read_text(args, 0) };
    let Some(path) = path else {
        unsafe {
            write_ptr(ret, ptr::null_mut());
        }
        return;
    };

    let result = match image::open(&path) {
        Ok(img) => {
            let rgba = img.to_rgba8();
            let data = rgba.into_raw();
            unsafe { alloc_array_from_bytes(&data) as *mut c_void }
        }
        Err(_) => ptr::null_mut(),
    };

    unsafe { write_ptr(ret, result) };
}

#[no_mangle]
pub extern "C" fn marmot_image_write_rgba(args: *mut *mut c_void, ret: *mut c_void) {
    let path = unsafe { read_text(args, 0) };
    let Some(path) = path else {
        unsafe {
            write_i64(ret, 0);
        }
        return;
    };

    let width_i64 = unsafe { read_i64(args, 1) };
    let height_i64 = unsafe { read_i64(args, 2) };
    if width_i64 <= 0 || height_i64 <= 0 {
        unsafe {
            write_i64(ret, 0);
        }
        return;
    }

    if width_i64 > u32::MAX as i64 || height_i64 > u32::MAX as i64 {
        unsafe {
            write_i64(ret, 0);
        }
        return;
    }

    let width = width_i64 as u32;
    let height = height_i64 as u32;

    let data = unsafe { read_byte_array(args, 3) };
    let Some(data) = data else {
        unsafe {
            write_i64(ret, 0);
        }
        return;
    };

    let expected_len = (width as usize)
        .checked_mul(height as usize)
        .and_then(|value| value.checked_mul(4));
    if expected_len != Some(data.len()) {
        unsafe {
            write_i64(ret, 0);
        }
        return;
    }

    let format = image::ImageFormat::from_path(&path).unwrap_or(image::ImageFormat::Png);
    let result = image::save_buffer_with_format(
        &path,
        &data,
        width,
        height,
        image::ColorType::Rgba8,
        format,
    );

    let success = result.is_ok();
    unsafe {
        write_i64(ret, if success { 1 } else { 0 });
    }
}
