import { Validators } from '@angular/forms';

export const portRange = [Validators.min(1), Validators.max(65535)];
export const mgmtPortRange = [Validators.min(1024), Validators.max(65535)];
export const lifetimeRange = [Validators.min(0), Validators.max(2592000)];
export const required = Validators.required;
