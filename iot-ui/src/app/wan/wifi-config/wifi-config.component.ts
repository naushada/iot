import { Component, OnInit, OnDestroy } from '@angular/core';
import { Subscription } from 'rxjs';
import { FormBuilder, FormGroup } from '@angular/forms';
import { SessionService } from '../../../common/session.service';
import { ToastService } from '../../../common/toast.service';
import { DataStoreService } from '../../../common/datastore.service';
import { WifiNetwork } from '../../../common/app-globals';

@Component({
  selector: 'app-wifi-config',
  templateUrl: './wifi-config.component.html',
  styleUrls: ['./wifi-config.component.scss']
})
export class WifiConfigComponent implements OnInit, OnDestroy {
  form: FormGroup;
  networks: WifiNetwork[] = [];
  loading = true; saving = false; msg = '';
  private sub = new Subscription();
  private readonly KEYS = [
    'wifi.iface', 'wifi.wpa.path', 'wifi.ctrl.dir',
    'wifi.scan.interval.sec', 'wifi.scan.max.results',
    'wifi.dhcp.client', 'wifi.networks',
  ];

    get isAdmin(): boolean { return this.session.isAdmin; }

  constructor(fb: FormBuilder, private session: SessionService,
              private toast: ToastService, private ds: DataStoreService) {
    this.form = fb.group({
      iface:            ['wlan0'],
      wpa_path:         ['/usr/sbin/wpa_supplicant'],
      ctrl_dir:         ['/run/wpa_supplicant'],
      scan_interval:    [60],
      scan_max_results: [20],
      dhcp_client:      ['auto'],
      networks_json:    ['[]'],
    });
  }

  ngOnInit(): void {
    // Paint instantly from the shared prefetched cache (no per-page round-trip),
    // then stay live off the appglobal store. Re-apply only while the form is
    // pristine so a late prefetch fills the fields without clobbering edits.
    this.applyData(this.ds.snapshot());
    this.loading = false;
    for (const k of this.KEYS)
      this.sub.add(this.ds.observe(k).subscribe(() => {
        if (!this.form.dirty) this.applyData(this.ds.snapshot());
      }));
  }

  ngOnDestroy(): void { this.sub.unsubscribe(); }

  private applyData(d: Record<string, unknown>): void {
    this.form.patchValue({
      iface:            d['wifi.iface']             ?? this.form.value.iface,
      wpa_path:         d['wifi.wpa.path']          ?? this.form.value.wpa_path,
      ctrl_dir:         d['wifi.ctrl.dir']          ?? this.form.value.ctrl_dir,
      scan_interval:    d['wifi.scan.interval.sec'] ?? this.form.value.scan_interval,
      scan_max_results: d['wifi.scan.max.results']  ?? this.form.value.scan_max_results,
      dhcp_client:      d['wifi.dhcp.client']       ?? this.form.value.dhcp_client,
      networks_json:    d['wifi.networks']          ?? this.form.value.networks_json,
    });
    try { this.networks = JSON.parse(this.form.get('networks_json')!.value || '[]'); }
    catch { this.networks = []; }
  }

  addNetwork(): void {
    this.networks.push({ ssid: '', psk: '', priority: 0, key_mgmt: 'WPA2-PSK' });
  }

  removeNetwork(i: number): void { this.networks.splice(i, 1); }

  save(): void {
    this.saving = true; this.msg = '';
    this.form.patchValue({ networks_json: JSON.stringify(this.networks) });
    const v = this.form.value;
    this.ds.write([
      { key: 'wifi.iface',               value: v.iface },
      { key: 'wifi.wpa.path',            value: v.wpa_path },
      { key: 'wifi.ctrl.dir',            value: v.ctrl_dir },
      { key: 'wifi.scan.interval.sec',   value: v.scan_interval },
      { key: 'wifi.scan.max.results',    value: v.scan_max_results },
      { key: 'wifi.dhcp.client',         value: v.dhcp_client },
      { key: 'wifi.networks',            value: v.networks_json },
    ]).subscribe({
      next: (r) => { this.saving = false; if(r.ok) this.toast.success('WiFi config saved'); else this.toast.error(r.err||'Save failed'); },
      error: () => { this.saving = false; this.toast.error('Save failed'); }
    });
  }
}
