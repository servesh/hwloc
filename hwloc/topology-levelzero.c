/*
 * Copyright © 2020-2023 Inria.  All rights reserved.
 * See COPYING in top-level directory.
 */

#include "private/autogen/config.h"
#include "hwloc.h"
#include "hwloc/plugins.h"

/* private headers allowed for convenience because this plugin is built within hwloc */
#include "private/misc.h"
#include "private/debug.h"

#include <level_zero/ze_api.h>
#include <level_zero/zes_api.h>

struct hwloc_osdev_array {
  unsigned nr, nr_allocated;
  hwloc_obj_t *objs;
};

static void
hwloc__levelzero_osdev_array_init(struct hwloc_osdev_array *array)
{
  array->nr = 0;
  array->nr_allocated = 0;
  array->objs = NULL;
}

static int
hwloc__levelzero_osdev_array_add(struct hwloc_osdev_array *array,
                                 hwloc_obj_t new)
{
  if (array->nr_allocated == array->nr) {
    unsigned new_nr_allocated = array->nr_allocated + 40; /* enough for 8 devices + 4 subdevices each on first allocation */
    hwloc_obj_t *tmp = realloc(array->objs, new_nr_allocated * sizeof(*array->objs));
    if (!tmp)
      /* the object won't be added */
      return -1;
    array->nr_allocated = new_nr_allocated;
    array->objs = tmp;
  }
  array->objs[array->nr++] = new;
  return 0;
}

static void
hwloc__levelzero_osdev_array_free(struct hwloc_osdev_array *array)
{
  free(array->objs);
}

static int
hwloc__levelzero_osdev_array_find(struct hwloc_osdev_array *array,
                                  hwloc_obj_t osdev)
{
  unsigned i;
  for(i=0; i<array->nr; i++)
    if (array->objs[i] == osdev)
      return (int)i;
  return -1;
}

static void
hwloc__levelzero_properties_get(ze_device_handle_t zeh, zes_device_handle_t zesh,
                                hwloc_obj_t osdev, ze_device_properties_t *prop)
{
  ze_result_t res;
  ze_device_properties_t _prop;
  zes_device_properties_t prop2;
  int is_subdevice = 0;

  if (!prop) {
    /* no properties were given, get ours */
    memset(&_prop, 0, sizeof(_prop));
    res = zeDeviceGetProperties(zeh, &_prop);
    if (res == ZE_RESULT_SUCCESS)
      prop = &_prop;
  }
  if (prop) {
    /* name is the model name followed by the deviceID
     * flags 1<<0 means integrated (vs discrete).
     */
    char tmp[64];
    char uuid[ZE_MAX_DEVICE_UUID_SIZE*2+1];
    unsigned i;
    const char *type;

    switch (prop->type) {
    case ZE_DEVICE_TYPE_GPU: type = "GPU"; break;
    case ZE_DEVICE_TYPE_CPU: type = "CPU"; break;
    case ZE_DEVICE_TYPE_FPGA: type = "FPGA"; break;
    case ZE_DEVICE_TYPE_MCA: type = "MCA"; break;
    case ZE_DEVICE_TYPE_VPU: type = "VPU"; break;
    default:
      if (HWLOC_SHOW_ALL_ERRORS())
        fprintf(stderr, "hwloc/levelzero: unexpected device type %u\n", (unsigned) prop->type);
      type = "Unknown";
    }
    hwloc_obj_add_info(osdev, "LevelZeroDeviceType", type);
    snprintf(tmp, sizeof(tmp), "%u", prop->numSlices);
    hwloc_obj_add_info(osdev, "LevelZeroNumSlices", tmp);
    snprintf(tmp, sizeof(tmp), "%u", prop->numSubslicesPerSlice);
    hwloc_obj_add_info(osdev, "LevelZeroNumSubslicesPerSlice", tmp);
    snprintf(tmp, sizeof(tmp), "%u", prop->numEUsPerSubslice);
    hwloc_obj_add_info(osdev, "LevelZeroNumEUsPerSubslice", tmp);
    snprintf(tmp, sizeof(tmp), "%u", prop->numThreadsPerEU);
    hwloc_obj_add_info(osdev, "LevelZeroNumThreadsPerEU", tmp);

    for(i=0; i<ZE_MAX_DEVICE_UUID_SIZE; i++)
      snprintf(uuid+2*i, 3, "%02x", prop->uuid.id[i]);
    hwloc_obj_add_info(osdev, "LevelZeroUUID", uuid);

    if (prop->flags & ZE_DEVICE_PROPERTY_FLAG_SUBDEVICE)
      is_subdevice = 1;
  }

  if (is_subdevice)
    /* sysman API on subdevice returns the same as root device, and we don't need those duplicate attributes */
    return;

  /* try to get additional info from sysman if enabled */
  memset(&prop2, 0, sizeof(prop2));
  res = zesDeviceGetProperties(zesh, &prop2);
  if (res == ZE_RESULT_SUCCESS) {
    /* old implementations may return "Unknown", recent may return "unknown" */
    if (strcasecmp((const char *) prop2.vendorName, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroVendor", (const char *) prop2.vendorName);
    if (strcasecmp((const char *) prop2.modelName, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroModel", (const char *) prop2.modelName);
    if (strcasecmp((const char *) prop2.brandName, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroBrand", (const char *) prop2.brandName);
    if (strcasecmp((const char *) prop2.serialNumber, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroSerialNumber", (const char *) prop2.serialNumber);
    if (strcasecmp((const char *) prop2.boardNumber, "Unknown"))
      hwloc_obj_add_info(osdev, "LevelZeroBoardNumber", (const char *) prop2.boardNumber);
  }
}

static void
hwloc__levelzero_cqprops_get(ze_device_handle_t zeh,
                             hwloc_obj_t osdev)
{
  ze_command_queue_group_properties_t *cqprops;
  unsigned nr_cqprops = 0;
  ze_result_t res;

  res = zeDeviceGetCommandQueueGroupProperties(zeh, &nr_cqprops, NULL);
  if (res != ZE_RESULT_SUCCESS || !nr_cqprops)
    return;

  cqprops = malloc(nr_cqprops * sizeof(*cqprops));
  if (cqprops) {
    res = zeDeviceGetCommandQueueGroupProperties(zeh, &nr_cqprops, cqprops);
    if (res == ZE_RESULT_SUCCESS) {
      unsigned k;
      char tmp[32];
      snprintf(tmp, sizeof(tmp), "%u", nr_cqprops);
      hwloc_obj_add_info(osdev, "LevelZeroCQGroups", tmp);
      for(k=0; k<nr_cqprops; k++) {
        char name[32];
        snprintf(name, sizeof(name), "LevelZeroCQGroup%u", k);
        snprintf(tmp, sizeof(tmp), "%u*0x%lx", (unsigned) cqprops[k].numQueues, (unsigned long) cqprops[k].flags);
        hwloc_obj_add_info(osdev, name, tmp);
      }
    }
    free(cqprops);
  }
}

static int
hwloc__levelzero_memory_get(zes_device_handle_t zesh,
                            hwloc_obj_t root_osdev,
                            unsigned nr_osdevs, hwloc_obj_t *sub_osdevs)
{
  zes_mem_handle_t *mh;
  uint32_t nr_mems;
  ze_result_t res;
  unsigned long long totalHBMkB = 0;
  unsigned long long totalDDRkB = 0;

  nr_mems = 0;
  res = zesDeviceEnumMemoryModules(zesh, &nr_mems, NULL);
  if (res != ZE_RESULT_SUCCESS)
    return -1; /* notify that sysman failed */

  hwloc_debug("L0/Sysman: found %u memory modules in osdev %s\n",
              nr_mems, root_osdev->name);
  if (!nr_mems)
    return 0;

  mh = malloc(nr_mems * sizeof(*mh));
  if (mh) {
    res = zesDeviceEnumMemoryModules(zesh, &nr_mems, mh);
    if (res == ZE_RESULT_SUCCESS) {
      unsigned m;
      for(m=0; m<nr_mems; m++) {
        zes_mem_properties_t mprop;
        res = zesMemoryGetProperties(mh[m], &mprop);
        if (res == ZE_RESULT_SUCCESS) {
          const char *type;
          hwloc_obj_t osdev;
          char name[ZE_MAX_DEVICE_NAME+64], value[64];

          if (!mprop.physicalSize) {
            /* unknown, but memory state should have it */
            zes_mem_state_t s;
            res = zesMemoryGetState(mh[m], &s);
            if (res == ZE_RESULT_SUCCESS) {
              hwloc_debug("L0/Sysman: found size 0 for memory modules #%u, using memory state size instead\n", m);
              mprop.physicalSize = s.size;
            }
          }

          if (mprop.onSubdevice) {
            if (mprop.subdeviceId >= nr_osdevs || !nr_osdevs || !sub_osdevs) {
              if (HWLOC_SHOW_ALL_ERRORS())
                fprintf(stderr, "hwloc/levelzero: memory module #%u on unexpected subdeviceId #%u\n", m, mprop.subdeviceId);
              osdev = NULL; /* we'll ignore it but we'll still agregate its subdevice memories into totalHBM/DDRkB */
            } else {
              osdev = sub_osdevs[mprop.subdeviceId];
            }
          } else {
            osdev = root_osdev;
          }
          switch (mprop.type) {
          case ZES_MEM_TYPE_HBM:
            type = "HBM";
            totalHBMkB += mprop.physicalSize >> 10;
            break;
          case ZES_MEM_TYPE_DDR:
          case ZES_MEM_TYPE_DDR3:
          case ZES_MEM_TYPE_DDR4:
          case ZES_MEM_TYPE_DDR5:
          case ZES_MEM_TYPE_LPDDR:
          case ZES_MEM_TYPE_LPDDR3:
          case ZES_MEM_TYPE_LPDDR4:
          case ZES_MEM_TYPE_LPDDR5:
            type = "DDR";
            totalDDRkB += mprop.physicalSize >> 10;
            break;
          default:
            type = "Memory";
          }

          hwloc_debug("L0/Sysman: found %llu bytes type %s for osdev %s (onsub %d subid %u)\n",
                      (unsigned long long) mprop.physicalSize, type, osdev ? osdev->name : "NULL",
                      mprop.onSubdevice, mprop.subdeviceId);
          if (!osdev || !type || !mprop.physicalSize)
            continue;

          if (osdev != root_osdev) {
            /* set the subdevice memory immediately */
            snprintf(name, sizeof(name), "LevelZero%sSize", type);
            snprintf(value, sizeof(value), "%llu", (unsigned long long) mprop.physicalSize >> 10);
            hwloc_obj_add_info(osdev, name, value);
          }
        }
      }
    }
    free(mh);
  }

  /* set the root device memory at the end, once subdevice memories were agregated */
  if (totalHBMkB) {
    char value[64];
    snprintf(value, sizeof(value), "%llu", totalHBMkB);
    hwloc_obj_add_info(root_osdev, "LevelZeroHBMSize", value);
  }
  if (totalDDRkB) {
    char value[64];
    snprintf(value, sizeof(value), "%llu", totalDDRkB);
    hwloc_obj_add_info(root_osdev, "LevelZeroDDRSize", value);
  }

  return 0;
}

struct hwloc_levelzero_ports {
  unsigned nr_allocated;
  unsigned nr;
  struct hwloc_levelzero_port {
    hwloc_obj_t osdev;
    hwloc_obj_t parent_osdev;
    zes_fabric_port_properties_t props;
    zes_fabric_port_state_t state;
  } *ports;
};

static void
hwloc__levelzero_ports_init(struct hwloc_levelzero_ports *hports)
{
  hports->nr_allocated = 0;
  hports->nr = 0;
  hports->ports = NULL;

  free(hports->ports);
}

static void
hwloc__levelzero_ports_get(zes_device_handle_t zesh,
                           hwloc_obj_t root_osdev,
                           unsigned nr_sub_osdevs, hwloc_obj_t *sub_osdevs,
                           struct hwloc_levelzero_ports *hports)
{
  zes_fabric_port_handle_t *ports;
  uint32_t nr_new = 0;
  unsigned i;
  ze_result_t res;

  res = zesDeviceEnumFabricPorts(zesh, &nr_new, NULL);
  if (res != ZE_RESULT_SUCCESS || !nr_new)
    return;
  hwloc_debug("L0 device %s has %u fabric ports\n", root_osdev->name, nr_new);

  if (hports->nr_allocated - hports->nr < nr_new) {
    /* we must extend the array */
    struct hwloc_levelzero_port *tmp;
    uint32_t new_nr_allocated;
    /* Extend the array by 8x the number of ports in this device.
     * This should mean a single allocation per 8 devices.
     */
    new_nr_allocated = hports->nr_allocated + 8*nr_new;
    tmp = realloc(hports->ports, new_nr_allocated * sizeof(*hports->ports));
    if (!tmp)
      return;
    hports->ports = tmp;
    hports->nr_allocated = new_nr_allocated;
  }

  ports = malloc(nr_new * sizeof(*ports));
  if (!ports)
    return;
  res = zesDeviceEnumFabricPorts(zesh, &nr_new, ports);

  for(i=0; i<nr_new; i++) {
    unsigned id = hports->nr;
    res = zesFabricPortGetProperties(ports[i], &hports->ports[id].props);
    if (res != ZE_RESULT_SUCCESS)
      continue;
    if (hports->ports[id].props.onSubdevice) {
      hwloc_debug("  port #%u is on subdevice #%u\n",
                  i, hports->ports[id].props.subdeviceId);
      if (hports->ports[id].props.subdeviceId < nr_sub_osdevs) {
        hports->ports[id].osdev = sub_osdevs[hports->ports[id].props.subdeviceId];
        hports->ports[id].parent_osdev = root_osdev;
      } else {
        hwloc_debug("    no such subdevice exists, ignoring\n");
        continue;
      }
    } else {
      hports->ports[id].osdev = root_osdev;
      hports->ports[id].parent_osdev = NULL;
    }

    res = zesFabricPortGetState(ports[i], &hports->ports[id].state);
    if (res != ZE_RESULT_SUCCESS)
      continue;
    if (hports->ports[id].state.status != ZES_FABRIC_PORT_STATUS_HEALTHY
        && hports->ports[id].state.status != ZES_FABRIC_PORT_STATUS_DEGRADED) {
      hwloc_debug("    status (%u)  isn't healthy or degraded\n",
                  (unsigned)hports->ports[id].state.status);
      continue;
    }
    hwloc_debug("    status is %s, this will be our hport #%u\n",
                hports->ports[id].state.status == ZES_FABRIC_PORT_STATUS_HEALTHY ? "healthy" : "degraded",
                id);

    hwloc_debug("    portId %u-%u-%u is connected to remote portId %u-%u-%u\n",
                hports->ports[id].props.portId.fabricId,
                hports->ports[id].props.portId.attachId,
                hports->ports[id].props.portId.portNumber,
                hports->ports[id].state.remotePortId.fabricId,
                hports->ports[id].state.remotePortId.attachId,
                hports->ports[id].state.remotePortId.portNumber);
    hports->nr++;
  }

  free(ports);
}

static int
hwloc__levelzero_devices_get(struct hwloc_topology *topology,
                             struct hwloc_osdev_array *oarray,
                             struct hwloc_levelzero_ports *hports)
{
  ze_driver_handle_t *drh;
  zes_driver_handle_t *zesdrh;
  uint32_t nbdrivers, i, k, zeidx, added = 0;
  ze_result_t res;

  nbdrivers = 0;
  res = zeDriverGet(&nbdrivers, NULL);
  if (res != ZE_RESULT_SUCCESS || !nbdrivers)
    return 0;
  i = 0;
  res = zesDriverGet(&i, NULL);
  if (res != ZE_RESULT_SUCCESS || i != nbdrivers) {
    if (HWLOC_SHOW_ALL_ERRORS())
      fprintf(stderr, "hwloc/levelzero: zesDriverGet returned %x and found %u ZES drivers vs %u ZE drivers\n", res, i, nbdrivers);
    return 0;
  }
  hwloc_debug("found %u ZE/ZES drivers\n", (unsigned) nbdrivers);

  drh = malloc(nbdrivers * sizeof(*drh));
  zesdrh = malloc(nbdrivers * sizeof(*zesdrh));
  if (!drh || !zesdrh) {
    free(drh);
    free(zesdrh);
    return 0;
  }
  res = zeDriverGet(&nbdrivers, drh);
  if (res != ZE_RESULT_SUCCESS) {
    free(drh);
    free(zesdrh);
    return 0;
  }
  res = zesDriverGet(&nbdrivers, zesdrh);
  if (res != ZE_RESULT_SUCCESS) {
    free(drh);
    free(zesdrh);
    return 0;
  }

  zeidx = 0;
  for(i=0; i<nbdrivers; i++) {
    uint32_t nbdevices, j;
    ze_device_handle_t *dvh;
    char buffer[13];

    nbdevices = 0;
    res = zeDeviceGet(drh[i], &nbdevices, NULL);
    if (res != ZE_RESULT_SUCCESS || !nbdevices)
      continue;
    hwloc_debug("hwloc/L0: found %u devices in driver #%u\n", (unsigned) nbdevices, (unsigned) i);
    dvh = malloc(nbdevices * sizeof(*dvh));
    if (!dvh)
      continue;
    res = zeDeviceGet(drh[i], &nbdevices, dvh);
    if (res != ZE_RESULT_SUCCESS) {
      free(dvh);
      continue;
    }

#if 0
    /* No interesting info attr to get from driver properties for now.
     * 2022/12/09: Driver UUID is driver version (major>>24|minor>>16|build) | a-uuid-timestamp>>32
     *  hence it's not stable across multiple runs
     */
    ze_driver_properties_t drprop;
    res = zeDriverGetProperties(drh[i], &drprop);
#endif

    for(j=0; j<nbdevices; j++) {
      ze_device_handle_t zeh = dvh[j];
      zes_device_handle_t zesh;
      ze_device_handle_t *subzehs = NULL;
      uint32_t nr_subdevices;
      hwloc_obj_t osdev, parent, *subosdevs = NULL;
      ze_device_properties_t props;
      zes_uuid_t uuid;
      ze_bool_t onSubdevice = 0;
      uint32_t subdeviceId = 0;

      res = zeDeviceGetProperties(zeh, &props);
      if (res != ZE_RESULT_SUCCESS) {
        if (HWLOC_SHOW_ALL_ERRORS())
          fprintf(stderr, "hwloc/levelzero: zeDeviceGetProperties() failed %x, skipping driver %u device %u\n",
                  res, i, j);
        continue;
      }
      memcpy(uuid.id, props.uuid.id, ZE_MAX_DEVICE_UUID_SIZE);
      res = zesDriverGetDeviceByUuidExp(zesdrh[i], uuid, &zesh, &onSubdevice, &subdeviceId);
      if (res != ZE_RESULT_SUCCESS) {
        if (HWLOC_SHOW_ALL_ERRORS())
          fprintf(stderr, "hwloc/levelzero: zesDriverGetDeviceByUuidExp() failed %x, skipping driver %u device %u\n",
                  res, i, j);
        continue;
      }

      osdev = hwloc_alloc_setup_object(topology, HWLOC_OBJ_OS_DEVICE, HWLOC_UNKNOWN_INDEX);
      snprintf(buffer, sizeof(buffer), "ze%u", zeidx); // ze0d0 ?
      osdev->name = strdup(buffer);
      osdev->depth = HWLOC_TYPE_DEPTH_UNKNOWN;
      osdev->attr->osdev.types = HWLOC_OBJ_OSDEV_COPROC | HWLOC_OBJ_OSDEV_GPU;
      osdev->subtype = strdup("LevelZero");

      snprintf(buffer, sizeof(buffer), "%u", i);
      hwloc_obj_add_info(osdev, "LevelZeroDriverIndex", buffer);
      snprintf(buffer, sizeof(buffer), "%u", j);
      hwloc_obj_add_info(osdev, "LevelZeroDriverDeviceIndex", buffer);

      hwloc__levelzero_properties_get(zeh, zesh, osdev, &props);

      hwloc__levelzero_cqprops_get(zeh, osdev);

      nr_subdevices = 0;
      res = zeDeviceGetSubDevices(zeh, &nr_subdevices, NULL);
      /* returns ZE_RESULT_ERROR_INVALID_ARGUMENT if there are no subdevices */
      if (res == ZE_RESULT_SUCCESS && nr_subdevices > 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%u", nr_subdevices);
        hwloc_obj_add_info(osdev, "LevelZeroSubdevices", tmp);
        subzehs = malloc(nr_subdevices * sizeof(*subzehs));
        subosdevs = malloc(nr_subdevices * sizeof(*subosdevs));
        if (subosdevs && subzehs) {
          zeDeviceGetSubDevices(zeh, &nr_subdevices, subzehs);
          for(k=0; k<nr_subdevices; k++) {
            ze_device_handle_t subzeh = subzehs[k];
            zes_device_handle_t subzesh = NULL;

            res = zeDeviceGetProperties(subzeh, &props);
            if (res != ZE_RESULT_SUCCESS) {
              if (HWLOC_SHOW_ALL_ERRORS())
                fprintf(stderr, "hwloc/levelzero: subdevice zeDeviceGetProperties() failed %x, skipping driver %u device %u\n",
                        res, i, j);
              continue;
            }
            memcpy(uuid.id, props.uuid.id, ZE_MAX_DEVICE_UUID_SIZE);
            res = zesDriverGetDeviceByUuidExp(zesdrh[i], uuid, &subzesh, &onSubdevice, &subdeviceId);
            if (res != ZE_RESULT_SUCCESS) {
              if (HWLOC_SHOW_ALL_ERRORS())
                fprintf(stderr, "hwloc/levelzero: subdevice zesDriverGetDeviceByUuidExp() failed %x, skipping driver %u device %u\n",
                        res, i, j);
              continue;
            }

            subosdevs[k] = hwloc_alloc_setup_object(topology, HWLOC_OBJ_OS_DEVICE, HWLOC_UNKNOWN_INDEX);
            snprintf(tmp, sizeof(tmp), "ze%u.%u", zeidx, k);
            subosdevs[k]->name = strdup(tmp);
            subosdevs[k]->depth = HWLOC_TYPE_DEPTH_UNKNOWN;
            subosdevs[k]->attr->osdev.types = HWLOC_OBJ_OSDEV_COPROC | HWLOC_OBJ_OSDEV_GPU;
            subosdevs[k]->subtype = strdup("LevelZero");
            snprintf(tmp, sizeof(tmp), "%u", k);
            hwloc_obj_add_info(subosdevs[k], "LevelZeroSubdeviceID", tmp);

            hwloc__levelzero_properties_get(subzeh, subzesh, subosdevs[k], NULL);

            hwloc__levelzero_cqprops_get(subzeh, subosdevs[k]);
          }
        } else {
          free(subosdevs);
          free(subzehs);
          subosdevs = NULL;
          subzehs = NULL;
          nr_subdevices = 0;
        }
      }

      /* get all memory info at once */
      hwloc__levelzero_memory_get(zesh, osdev, nr_subdevices, subosdevs);

      /* get all ports info at once */
      if (!(hwloc_topology_get_flags(topology) & HWLOC_TOPOLOGY_FLAG_NO_DISTANCES))
        hwloc__levelzero_ports_get(zesh, osdev, nr_subdevices, subosdevs, hports);

      parent = NULL;
#ifdef HWLOC_HAVE_ZEDEVICEPCIGETPROPERTIESEXT
      { /* try getting PCI BDF+speed from core extension */
        ze_pci_ext_properties_t ext_pci;
        ext_pci.stype =  ZE_STRUCTURE_TYPE_PCI_EXT_PROPERTIES;
        ext_pci.pNext = NULL;
        res = zeDevicePciGetPropertiesExt(zeh, &ext_pci);
        if (res == ZE_RESULT_SUCCESS) {
          parent = hwloc_pci_find_parent_by_busid(topology,
                                                  ext_pci.address.domain,
                                                  ext_pci.address.bus,
                                                  ext_pci.address.device,
                                                  ext_pci.address.function);
          if (parent && parent->type == HWLOC_OBJ_PCI_DEVICE) {
            if (ext_pci.maxSpeed.maxBandwidth > 0)
              parent->attr->pcidev.linkspeed = ((float)ext_pci.maxSpeed.maxBandwidth)/1000/1000/1000;
          }
        }
      }
#endif /* HWLOC_HAVE_LEVELZERO_CORE_PCI_EXT */
      if (!parent) {
        /* try getting PCI BDF+speed from sysman */
        zes_pci_properties_t pci;
        res = zesDevicePciGetProperties(zesh, &pci);
        if (res == ZE_RESULT_SUCCESS) {
          parent = hwloc_pci_find_parent_by_busid(topology,
                                                  pci.address.domain,
                                                  pci.address.bus,
                                                  pci.address.device,
                                                  pci.address.function);
          if (parent && parent->type == HWLOC_OBJ_PCI_DEVICE) {
            if (pci.maxSpeed.maxBandwidth > 0)
              parent->attr->pcidev.linkspeed = ((float)pci.maxSpeed.maxBandwidth)/1000/1000/1000;
          }
        }
      }
      if (!parent)
        parent = hwloc_get_root_obj(topology);

      /* WARNING: parent and its subdevices are inserted together in oarray[].
       * this is required when filling the BW matrix at the end of hwloc__levelzero_ports_connect()
       */
      hwloc_insert_object_by_parent(topology, parent, osdev);
      hwloc__levelzero_osdev_array_add(oarray, osdev);
      added++;
      if (nr_subdevices) {
        for(k=0; k<nr_subdevices; k++)
          if (subosdevs[k]) {
            hwloc_insert_object_by_parent(topology, osdev, subosdevs[k]);
            hwloc__levelzero_osdev_array_add(oarray, subosdevs[k]);
            added++;
          }
        free(subosdevs);
        free(subzehs);
      }
      zeidx++;
    }

    free(dvh);
  }

  free(drh);
  free(zesdrh);

  return added;
}

static int
hwloc__levelzero_ports_add_xelink_bandwidth(struct hwloc_topology *topology,
                                            struct hwloc_osdev_array *oarray,
                                            hwloc_uint64_t *bws)
{
  void *handle;
  int err;

  handle = hwloc_backend_distances_add_create(topology, "XeLinkBandwidth",
                                              HWLOC_DISTANCES_KIND_FROM_OS|HWLOC_DISTANCES_KIND_MEANS_BANDWIDTH,
                                              0);
  if (!handle)
    goto out;

  err = hwloc_backend_distances_add_values(topology, handle, oarray->nr, oarray->objs, bws, 0);
  if (err < 0)
    goto out;
  /* arrays are now attached to the handle */
  oarray->objs = NULL;
  bws = NULL;

  err = hwloc_backend_distances_add_commit(topology, handle, 0 /* don't group GPUs */);
  if (err < 0)
    goto out;

  return 0;

 out:
  free(bws);
  return -1;
}

static int
hwloc__levelzero_ports_connect(struct hwloc_topology *topology,
                               struct hwloc_osdev_array *oarray,
                               struct hwloc_levelzero_ports *hports)
{
  hwloc_uint64_t *bws;
  unsigned i, j, k;
  int gotbw = 0;

  if (!hports->nr)
    return 0;

  bws = calloc(oarray->nr * oarray->nr, sizeof(*bws));
  if (!bws)
    return -1;

  for(i=0; i<hports->nr; i++) {
    if (hports->ports[i].state.status == ZES_FABRIC_PORT_STATUS_HEALTHY) {
      for(j=0; j<hports->nr; j++) {
        if (i==j)
          continue;
        if (hports->ports[i].state.remotePortId.fabricId == hports->ports[j].props.portId.fabricId
            && hports->ports[i].state.remotePortId.attachId == hports->ports[j].props.portId.attachId
            && hports->ports[i].state.remotePortId.portNumber == hports->ports[j].props.portId.portNumber) {
          int iindex, jindex;
          hwloc_debug("found L0 link model %s with %llu bit/s TX from hport #%u (osdev %s) to hport #%u (osdev %s)\n",
                      hports->ports[j].props.model,
                      (unsigned long long) hports->ports[i].state.rxSpeed.bitRate,
                      i,
                      hports->ports[i].osdev->name,
                      j,
                      hports->ports[j].osdev->name);
          /* only keep XeLink for now */
          if (strcmp(hports->ports[j].props.model, "XeLink"))
            continue;
          iindex = hwloc__levelzero_osdev_array_find(oarray, hports->ports[i].osdev);
          jindex = hwloc__levelzero_osdev_array_find(oarray, hports->ports[j].osdev);
          if (iindex<0 || jindex<0)
            continue;
          bws[iindex*oarray->nr+jindex] += hports->ports[i].state.rxSpeed.bitRate >> 20; /* MB/s */
          if (hports->ports[i].parent_osdev && hports->ports[j].parent_osdev) {
            /* also accumulate in root devices */
            iindex = hwloc__levelzero_osdev_array_find(oarray, hports->ports[i].parent_osdev);
            jindex = hwloc__levelzero_osdev_array_find(oarray, hports->ports[j].parent_osdev);
            if (iindex<0 || jindex<0)
              continue;
            bws[iindex*oarray->nr+jindex] += hports->ports[i].state.rxSpeed.bitRate >> 20; /* MB/s */
          }
          gotbw++;
        }
      }
    }
  }
  if (!gotbw) {
    free(bws);
    return 0;
  }

  /* Add very high artifical values on the diagonal since local is faster than remote.
   * Use 1TB/s for local, it somehow matches the HBM.
   * Also between subdevices of the same parent, and between subdevice and parent (TODO improve this)
   *
   * WARNING: this assumes that parent and subdevices were added together
   * in oarray[] in hwloc_levelzero_discover().
   */
  for(i=0; i<oarray->nr; i++) {
    unsigned nrsd = 0;
    const char *s = hwloc_obj_get_info_by_name(oarray->objs[i], "LevelZeroSubdevices");
    if (s)
      nrsd = atoi(s);
    /* set 1000000 for all devices between i and i+nrsd */
    for(j=0; j<nrsd+1; j++)
      for(k=0; k<nrsd+1; k++)
        bws[(i+j)*oarray->nr+(i+k)] = 1000000;
    /* skip subdevices */
    i += nrsd;
  }

  return hwloc__levelzero_ports_add_xelink_bandwidth(topology, oarray, bws);
}

static void
hwloc__levelzero_ports_destroy(struct hwloc_levelzero_ports *hports)
{
  free(hports->ports);
}

static int
hwloc_levelzero_discover(struct hwloc_backend *backend, struct hwloc_disc_status *dstatus)
{
  /*
   * This backend uses the underlying OS.
   * However we don't enforce topology->is_thissystem so that
   * we may still force use this backend when debugging with !thissystem.
   */

  struct hwloc_topology *topology = backend->topology;
  enum hwloc_type_filter_e filter;
  int added;
  ze_result_t res;
  struct hwloc_osdev_array oarray;
  struct hwloc_levelzero_ports hports;

  assert(dstatus->phase == HWLOC_DISC_PHASE_IO);

  hwloc_topology_get_type_filter(topology, HWLOC_OBJ_OS_DEVICE, &filter);
  if (filter == HWLOC_TYPE_FILTER_KEEP_NONE)
    return 0;

  hwloc__levelzero_osdev_array_init(&oarray);

  hwloc__levelzero_ports_init(&hports);

  res = zeInit(0);
  if (res != ZE_RESULT_SUCCESS) {
    if (HWLOC_SHOW_ALL_ERRORS()) {
      fprintf(stderr, "hwloc/levelzero: Failed to initialize in zeInit(): 0x%x\n", (unsigned)res);
    }
    return 0;
  }

  res = zesInit(0);
  if (res != ZE_RESULT_SUCCESS) {
    if (HWLOC_SHOW_ALL_ERRORS()) {
      fprintf(stderr, "hwloc/levelzero: Failed to initialize in zesInit(): 0x%x\n", (unsigned)res);
    }
    return 0;
  }

  added = hwloc__levelzero_devices_get(topology, &oarray, &hports);
  hwloc__levelzero_ports_connect(topology, &oarray, &hports);
  hwloc__levelzero_ports_destroy(&hports);

  hwloc__levelzero_osdev_array_free(&oarray);

  if (added > 0)
    hwloc_modify_infos(hwloc_topology_get_infos(topology), HWLOC_MODIFY_INFOS_OP_ADD, "Backend", "LevelZero");
  return 0;
}

static struct hwloc_backend *
hwloc_levelzero_component_instantiate(struct hwloc_topology *topology,
                                      struct hwloc_disc_component *component,
                                      unsigned excluded_phases __hwloc_attribute_unused,
                                      const void *_data1 __hwloc_attribute_unused,
                                      const void *_data2 __hwloc_attribute_unused,
                                      const void *_data3 __hwloc_attribute_unused)
{
  struct hwloc_backend *backend;

  backend = hwloc_backend_alloc(topology, component);
  if (!backend)
    return NULL;
  backend->discover = hwloc_levelzero_discover;
  return backend;
}

static struct hwloc_disc_component hwloc_levelzero_disc_component = {
  "levelzero",
  HWLOC_DISC_PHASE_IO,
  HWLOC_DISC_PHASE_GLOBAL,
  hwloc_levelzero_component_instantiate,
  10, /* after pci */
  1,
  NULL
};

static int
hwloc_levelzero_component_init(unsigned long flags)
{
  if (flags)
    return -1;
  if (hwloc_plugin_check_namespace("levelzero", "hwloc_backend_alloc") < 0)
    return -1;
  return 0;
}

#ifdef HWLOC_INSIDE_PLUGIN
HWLOC_DECLSPEC extern const struct hwloc_component hwloc_levelzero_component;
#endif

const struct hwloc_component hwloc_levelzero_component = {
  HWLOC_COMPONENT_ABI,
  hwloc_levelzero_component_init, NULL,
  HWLOC_COMPONENT_TYPE_DISC,
  0,
  &hwloc_levelzero_disc_component
};
