/*
 * Reversible mutations of Sony-owned gadget state.
 *
 * This remains in the stage3_serial translation unit so every tracked pointer
 * is private to the module while keeping teardown policy separate from USB
 * injection.
 */
#define SIEUSB_MAX_HID_PATCHES (SIEUSB_MAX_FUNCS * 3)
#define SIEUSB_MAX_EPS 32

struct s3_sony_function_patch {
  struct usb_function *function;
  int (*set_alt)(struct usb_function *, unsigned, unsigned);
  void (*disable)(struct usb_function *);
};

struct s3_hid_patch {
  struct usb_interface_descriptor *descriptor;
  u8 interface_class;
};

struct s3_ep_claim_patch {
  struct usb_ep *ep;
  bool claimed;
};

static struct s3_sony_function_patch
    sony_function_patches[SIEUSB_MAX_FUNCS];
static unsigned int sony_function_patch_count;
static struct s3_hid_patch sony_hid_patches[SIEUSB_MAX_HID_PATCHES];
static unsigned int sony_hid_patch_count;
static struct s3_ep_claim_patch ep_claim_patches[SIEUSB_MAX_EPS];
static unsigned int ep_claim_patch_count;
static enum usb_device_speed original_gadget_max_speed;
static bool gadget_max_speed_changed;
static int *panic_on_oops_ptr;
static int original_panic_on_oops;
static int *panic_timeout_ptr;
static int original_panic_timeout;
static bool panic_state_changed;
static unsigned char original_acm_next_interface_id;
static bool acm_interface_state_saved;
static void s3_cloak_hid_descriptors(
    struct usb_function *function,
    struct usb_descriptor_header **descriptors,
    const char *speed) {
  struct usb_descriptor_header **desc;

  if (!descriptors)
    return;

  for (desc = descriptors; *desc; desc++) {
    struct usb_interface_descriptor *interface;

    if ((*desc)->bDescriptorType != USB_DT_INTERFACE)
      continue;
    interface = (struct usb_interface_descriptor *)*desc;
    if (interface->bInterfaceClass != USB_CLASS_HID)
      continue;
    if (sony_hid_patch_count >= ARRAY_SIZE(sony_hid_patches)) {
      printk(KERN_WARNING
             "stage3: HID patch table full; leaving %s descriptor unchanged\n",
             speed);
      continue;
    }

    sony_hid_patches[sony_hid_patch_count].descriptor = interface;
    sony_hid_patches[sony_hid_patch_count].interface_class =
        interface->bInterfaceClass;
    sony_hid_patch_count++;
    interface->bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    printk(KERN_INFO
           "stage3: cloaked %s HID class on %s; restoration recorded\n",
           speed, function->name ? function->name : "?");
  }
}

static void s3_restore_platform_mutations(void) {
  unsigned int i;

  for (i = sony_hid_patch_count; i > 0; i--) {
    struct s3_hid_patch *patch = &sony_hid_patches[i - 1];
    if (patch->descriptor)
      patch->descriptor->bInterfaceClass = patch->interface_class;
  }
  sony_hid_patch_count = 0;

  for (i = sony_function_patch_count; i > 0; i--) {
    struct s3_sony_function_patch *patch =
        &sony_function_patches[i - 1];
    if (!patch->function)
      continue;
    patch->function->set_alt = patch->set_alt;
    patch->function->disable = patch->disable;
  }
  sony_function_patch_count = 0;

  for (i = ep_claim_patch_count; i > 0; i--) {
    struct s3_ep_claim_patch *patch = &ep_claim_patches[i - 1];
    if (patch->ep)
      patch->ep->claimed = patch->claimed;
  }
  ep_claim_patch_count = 0;

  if (gadget_max_speed_changed && target_config) {
    struct usb_gadget *gadget = resolve_gadget(target_config);
    if (gadget)
      gadget->max_speed = original_gadget_max_speed;
  }
  gadget_max_speed_changed = false;

  if (panic_state_changed) {
    if (panic_on_oops_ptr)
      *panic_on_oops_ptr = original_panic_on_oops;
    if (panic_timeout_ptr)
      *panic_timeout_ptr = original_panic_timeout;
  }
  panic_state_changed = false;
  panic_on_oops_ptr = NULL;
  panic_timeout_ptr = NULL;
}

static void s3_restore_acm_interface_state(void) {
  unsigned int i;

  if (!acm_interface_state_saved || !target_config)
    return;

  for (i = 0; i < (unsigned int)active_acm_ports * 2; i++) {
    unsigned int id = original_acm_next_interface_id + i;
    if (id < MAX_CONFIG_INTERFACES)
      target_config->interface[id] = NULL;
  }
  probe_kernel_write(
      (char *)target_config + 88,
      &original_acm_next_interface_id,
      sizeof(original_acm_next_interface_id));
  acm_interface_state_saved = false;
}
