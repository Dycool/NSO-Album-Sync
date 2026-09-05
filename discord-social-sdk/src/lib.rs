#![allow(unsafe_code)]
#![deny(unsafe_op_in_unsafe_fn)]

//! Narrow FFI boundary for Discord Social SDK 1.10.18687.
//!
//! The parent application remains `#![forbid(unsafe_code)]`. Raw pointers and
//! foreign calls are contained in this crate and all SDK state stays behind the
//! C++ shim's internal mutexes.

use std::ffi::{CString, c_char, c_void};

#[repr(C)]
struct NativeActivity {
    name: *const c_char,
    details: *const c_char,
    state: *const c_char,
    start_seconds: u64,
    large_image: *const c_char,
    large_text: *const c_char,
    large_url: *const c_char,
    small_image: *const c_char,
    small_text: *const c_char,
}

unsafe extern "C" {
    fn nso_discord_social_create(application_id: u64) -> *mut c_void;
    fn nso_discord_social_destroy(handle: *mut c_void);
    fn nso_discord_social_self_test(handle: *mut c_void) -> i32;
    fn nso_discord_social_clear(handle: *mut c_void);
    fn nso_discord_social_update(handle: *mut c_void, activity: *const NativeActivity) -> i32;
}

#[derive(Debug, Clone, Copy)]
pub struct Activity<'a> {
    pub name: &'a str,
    pub details: &'a str,
    pub state: &'a str,
    pub start_seconds: u64,
    pub large_image: &'a str,
    pub large_text: &'a str,
    pub large_url: &'a str,
    pub small_image: &'a str,
    pub small_text: &'a str,
}

pub struct Client {
    handle: usize,
}

impl Client {
    #[must_use]
    pub fn new(application_id: u64) -> Option<Self> {
        // SAFETY: The shim accepts a plain integer and returns either null or a
        // heap-owned opaque ClientHolder pointer. Ownership transfers to `Self`.
        let handle = unsafe { nso_discord_social_create(application_id) };
        (!handle.is_null()).then_some(Self { handle: handle as usize })
    }

    #[must_use]
    pub fn self_test_runtime(&self) -> bool {
        // SAFETY: `self.handle` was created by the shim and remains owned by this
        // value until Drop; the C++ method synchronizes access internally.
        unsafe { nso_discord_social_self_test(self.raw()) != 0 }
    }

    pub fn clear(&self) {
        // SAFETY: `self.handle` is a live shim-owned ClientHolder. The call is
        // internally synchronized and does not retain Rust references.
        unsafe { nso_discord_social_clear(self.raw()) }
    }

    #[must_use]
    pub fn update(&self, activity: &Activity<'_>) -> bool {
        let Ok(name) = CString::new(activity.name) else { return false; };
        let Ok(details) = CString::new(activity.details) else { return false; };
        let Ok(state) = CString::new(activity.state) else { return false; };
        let Ok(large_image) = CString::new(activity.large_image) else { return false; };
        let Ok(large_text) = CString::new(activity.large_text) else { return false; };
        let Ok(large_url) = CString::new(activity.large_url) else { return false; };
        let Ok(small_image) = CString::new(activity.small_image) else { return false; };
        let Ok(small_text) = CString::new(activity.small_text) else { return false; };

        let native = NativeActivity {
            name: name.as_ptr(),
            details: details.as_ptr(),
            state: state.as_ptr(),
            start_seconds: activity.start_seconds,
            large_image: large_image.as_ptr(),
            large_text: large_text.as_ptr(),
            large_url: large_url.as_ptr(),
            small_image: small_image.as_ptr(),
            small_text: small_text.as_ptr(),
        };

        // SAFETY: All C strings and `native` live through this synchronous call.
        // The C++ shim copies them into Discord SDK value objects and retains no
        // pointer into Rust memory after returning.
        unsafe { nso_discord_social_update(self.raw(), &raw const native) != 0 }
    }

    fn raw(&self) -> *mut c_void {
        self.handle as *mut c_void
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        if self.handle == 0 {
            return;
        }
        // SAFETY: Drop runs exactly once for the unique opaque pointer returned
        // by `nso_discord_social_create`; the shim releases the client and joins
        // its callback thread before freeing the allocation.
        unsafe { nso_discord_social_destroy(self.raw()) }
        self.handle = 0;
    }
}
