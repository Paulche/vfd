// vi: sw=4 ts=4 noet:

/*
	Mnemonic:	vfd_rif.c
	Abstract:	These functions provide the request interface between VFd and 
				iplex.
	Author:		E. Scott Daniels
	Date:		11 October 2016 (broken out of main.c)
*/

/*
	Create our fifo and tuck the handle into the parm struct. Returns 0 on
	success and <0 on failure.
*/
static int vfd_init_fifo( parms_t* parms ) {
	if( !parms ) {
		return -1;
	}

	umask( 0 );
	parms->rfifo = rfifo_create( parms->fifo_path, 0666 );		//TODO -- set mode more sainly, but this runs as root, so regular users need to write to this thus open wide for now
	if( parms->rfifo == NULL ) {
		bleat_printf( 0, "ERR: unable to create request fifo (%s): %s", parms->fifo_path, strerror( errno ) );
		return -1;
	} else {
		bleat_printf( 0, "listening for requests via pipe: %s", parms->fifo_path );
	}

	return 0;
}

// ---------------------- validation --------------------------------------------------------------------------

/*
	Looks at the currently configured PF and determines whether or not the requested 
	traffic class percentages can be added without 'busting' the limits if we are in
	strict (no overscription) mode for the PF.  If we are in relaxed mode (oversub
	is allowed) then this function should not be called.

	Port is the PF number mapped from the pciid in the parm file.
	req_tcs is an array of the reqested tc percentages ordered traffic class 0-7.

	Return code of 0 indicates failure; non-zero is success.
	
*/
static int check_tcs( sriov_conf_t* conf, uint32_t port, uint8_t *tcpctgs ) {
	int	totals[MAX_TCS];			// current pct totals
	int	i;
	int j;
	int	rc = 1;						// return code; assume good

	memset( totals, 0, sizeof( totals ) );

	for( i = 0; i < MAX_VFS; i++ ) {				// sum the pctgs for each TC across all VFs
		if( conf->vfs[i].num >= 0 ) {				// active VF
			for( j = 0; j < MAX_TCS; j++ ) {
				totals[j] += conf->vfs[i].tc_pctgs[j];	// add in this total
			}
		}
	}

	for( i = 0; i < MAX_TCS; i++ ) {
		if( totals[i] + tcpctgs[i] > 100 ) {
			rc = 0;
			bleat_printf( 1, "requested traffic class percentage causes limit to be exceeded: tc=%d current=%d requested=%d", i, totals[i], tcpctgs[i] );
		}
	}

	return rc
}

//  --------------------- global config management ------------------------------------------------------------

/*
	Pull the list of pciids from the parms and set into the in memory configuration that
	is maintained. If this is called more than once, it will refuse to do anything.
*/
static void vfd_add_ports( parms_t* parms, sriov_conf_t* conf ) {
	static int called = 0;		// doesn't makes sense to do this more than once
	int i;
	int pidx = 0;				// port idx in conf list
	struct sriov_port_s* port;

	if( called )
		return;
	called = 1;
	
	for( i = 0; pidx < MAX_PORTS  && i < parms->npciids; i++, pidx++ ) {
		port->flags = 0;									// default all flags off

		port = &conf->ports[pidx];
		port->last_updated = ADDED;												// flag newly added so the nic is configured next go round
		snprintf( port->name, sizeof( port->name ), "port-%d",  i);				// TODO--- support getting a name from the config
		snprintf( port->pciid, sizeof( port->pciid ), "%s", parms->pciids[i].id );
		port->mtu = parms->pciids[i].mtu;

		if( parms->pciids[i].flags & PFF_LOOP_BACK ) {
			port->flags |= PF_LOOPBACK;											// enable VM->VM traffic without leaving nic
		}
		if( parms->pciids[i].flags & PFF_VF_OVERSUB ) {
			port->flags |= PF_OVERSUB;											// enable VM->VM traffic without leaving nic
		}

		port->num_mirrors = 0;
		port->num_vfs = 0;
		
		bleat_printf( 1, "add pciid to in memory config: %s mtu=%d", parms->pciids[i].id, parms->pciids[i].mtu );
	}

	conf->num_ports = pidx;
}

/*
	Add one of the virtualisation manager generated configuration files to a global 
	config struct passed in.  A small amount of error checking (vf id dup, etc) is 
	done, so the return is either 1 for success or 0 for failure. Errno is set only 
	if we can't open the file.  If reason is not NULL we'll create a message buffer 
	and drop the address there (caller must free).

	Future:
	It would make more sense for the config reader in lib to actually populate the
	actual vf struct rather than having to copy it, but because the port struct
	doesn't have dynamic VF structs (has a hard array), we need to read it into
	a separate location and copy it anyway, so the manual copy, rathter than a
	memcpy() is a minor annoyance.  Ultimately, the port should reference an
	array of pointers, and config should pull directly into a vf_s and if the
	parms are valid, then the pointer added to the list.
*/
static int vfd_add_vf( sriov_conf_t* conf, char* fname, char** reason ) {
	vf_config_t* vfc;					// raw vf config file contents	
	int	i;
	int j;
	int vidx;							// index into the vf array
	int	hole = -1;						// first hole in the list;
	struct sriov_port_s* port = NULL;	// reference to a single port in the config
	struct vf_s*	vf;					// point at the vf we need to fill in
	char mbuf[BUF_1K];					// message buffer if we fail
	int tot_vlans = 0;					// must count vlans and macs to ensure limit not busted
	int tot_macs = 0;
	

	if( conf == NULL || fname == NULL ) {
		bleat_printf( 0, "vfd_add_vf called with nil config or filename pointer" );
		if( reason ) {
			snprintf( mbuf, sizeof( mbuf), "internal mishap: config ptr was nil" );
			*reason = strdup( mbuf );
		}
		return 0;
	}

	if( (vfc = read_config( fname )) == NULL ) {
		snprintf( mbuf, sizeof( mbuf ), "unable to read config file: %s: %s", fname, errno > 0 ? strerror( errno ) : "unknown sub-reason" );
		bleat_printf( 1, "vfd_add_vf failed: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		return 0;
	}

	bleat_printf( 2, "add: config data: name: %s", vfc->name );
	bleat_printf( 2, "add: config data: pciid: %s", vfc->pciid );
	bleat_printf( 2, "add: config data: vfid: %d", vfc->vfid );

	if( vfc->pciid == NULL || vfc->vfid < 1 ) {
		snprintf( mbuf, sizeof( mbuf ), "unable to read or parse config file: %s", fname );
		bleat_printf( 1, "vfd_add_vf failed: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

	for( i = 0; i < conf->num_ports; i++ ) {						// find the port that this vf is attached to
		if( strcmp( conf->ports[i].pciid, vfc->pciid ) == 0 ) {	// match
			port = &conf->ports[i];
			break;
		}
	}

	if( port == NULL ) {
		snprintf( mbuf, sizeof( mbuf ), "%s: could not find port %s in the config", vfc->name, vfc->pciid );
		bleat_printf( 1, "vf not added: %s", mbuf );
		free_config( vfc );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		return 0;
	}

	for( i = 0; i < port->num_vfs; i++ ) {				// ensure ID is not already defined
		if( port->vfs[i].num < 0 ) {					// this is a hole
			if( hole < 0 ) {
				hole = i;								// we'll insert here
			}
		} else {
			if( port->vfs[i].num == vfc->vfid ) {			// dup, fail
				snprintf( mbuf, sizeof( mbuf ), "vfid %d already exists on port %s", vfc->vfid, vfc->pciid );
				bleat_printf( 1, "vf not added: %s", mbuf );
				if( reason ) {
					*reason = strdup( mbuf );
				}
				free_config( vfc );
				return 0;
			}

			tot_vlans += port->vfs[i].num_vlans;
			tot_macs += port->vfs[i].num_macs;
		}
	}

	if( hole >= 0 ) {			// set the index into the vf array based on first hole found, or no holes
		vidx = hole;
	} else {
		vidx = i;
	}

	if( vidx >= MAX_VFS || vfc->vfid < 1 || vfc->vfid > 31) {							// something is out of range
		snprintf( mbuf, sizeof( mbuf ), "max VFs already defined or vfid %d is out of range", vfc->vfid );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}

		free_config( vfc );
		return 0;
	}

	if( vfc->vfid >= port->nvfs_config ) {		// greater than the number configured
		snprintf( mbuf, sizeof( mbuf ), "vf %d is out of range; only %d VFs are configured on port %s", vfc->vfid, port->nvfs_config, port->pciid );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}

		free_config( vfc );
		return 0;
	}

	if( vfc->nvlans > MAX_VF_VLANS ) {				// more than allowed for a single VF
		snprintf( mbuf, sizeof( mbuf ), "number of vlans supplied (%d) exceeds the maximum (%d)", vfc->nvlans, MAX_VF_VLANS );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

	if( vfc->nvlans + tot_vlans > MAX_PF_VLANS ) { 			// would bust the total across the whole PF
		snprintf( mbuf, sizeof( mbuf ), "number of vlans supplied (%d) cauess total for PF to exceed the maximum (%d)", vfc->nvlans, MAX_PF_VLANS );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

	if( vfc->nmacs + tot_macs > MAX_PF_MACS ) { 			// would bust the total across the whole PF
		snprintf( mbuf, sizeof( mbuf ), "number of macs supplied (%d) cauess total for PF to exceed the maximum (%d)", vfc->nmacs, MAX_PF_MACS );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}


	if( vfc->nmacs > MAX_VF_MACS ) {
		snprintf( mbuf, sizeof( mbuf ), "number of macs supplied (%d) exceeds the maximum (%d)", vfc->nmacs, MAX_VF_MACS );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

	if( vfc->strip_stag  &&  vfc->nvlans > 1 ) {		// one vlan is allowed when stripping
		snprintf( mbuf, sizeof( mbuf ), "conflicting options: strip_stag may not be supplied with a list of vlan ids" );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

														// check vlan and mac arrays for duplicate values and bad things
	if( vfc->nvlans == 1 ) {							// no need for a dup check, just a range check
		if( vfc->vlans[0] < 1 || vfc->vlans[0] > 4095 ) {
			snprintf( mbuf, sizeof( mbuf ), "invalid vlan id: %d", vfc->vlans[0] );
			bleat_printf( 1, "vf not added: %s", mbuf );
			if( reason ) {
				*reason = strdup( mbuf );
			}
			free_config( vfc );
			return 0;
		}
	} else {
		for( i = 0; i < vfc->nvlans-1; i++ ) {
			if( vfc->vlans[i] < 1 || vfc->vlans[i] > 4095 ) {			// range check
				snprintf( mbuf, sizeof( mbuf ), "invalid vlan id: %d", vfc->vlans[i] );
				bleat_printf( 1, "vf not added: %s", mbuf );
				if( reason ) {
					*reason = strdup( mbuf );
				}
				free_config( vfc );
				return 0;
			}
	
			for( j = i+1; j < vfc->nvlans; j++ ) {
				if( vfc->vlans[i] == vfc->vlans[j] ) {					// dup check
					snprintf( mbuf, sizeof( mbuf ), "dupliate vlan in list: %d", vfc->vlans[i] );
					bleat_printf( 1, "vf not added: %s", mbuf );
					if( reason ) {
						*reason = strdup( mbuf );
					}
					free_config( vfc );
					return 0;
				}
			}
		}
	}

	if( vfc->nmacs == 1 ) {											// only need range check if one
		if( is_valid_mac_str( vfc->macs[0] ) < 0 ) {
			snprintf( mbuf, sizeof( mbuf ), "invalid mac in list: %s", vfc->macs[0] );
			bleat_printf( 1, "vf not added: %s", mbuf );
			if( reason ) {
				*reason = strdup( mbuf );
			}
			free_config( vfc );
			return 0;
		}
	} else {
		for( i = 0; i < vfc->nmacs-1; i++ ) {
			if( is_valid_mac_str( vfc->macs[i] ) < 0 ) {					// range check
				snprintf( mbuf, sizeof( mbuf ), "invalid mac in list: %s", vfc->macs[i] );
				bleat_printf( 1, "vf not added: %s", mbuf );
				if( reason ) {
					*reason = strdup( mbuf );
				}
				free_config( vfc );
				return 0;
			}

			for( j = i+1; j < vfc->nmacs; j++ ) {
				if( strcmp( vfc->macs[i], vfc->macs[j] ) == 0 ) {			// dup check
					snprintf( mbuf, sizeof( mbuf ), "dupliate mac in list: %s", vfc->macs[i] );
					bleat_printf( 1, "vf not added: %s", mbuf );
					if( reason ) {
						*reason = strdup( mbuf );
					}
					free_config( vfc );
					return 0;
				}
			}
		}
	}

	if( ! conf->flags & PF_OVERSUB ) {						// if in strict mode, ensure TC amounts can be added to current settings without busting ceil
		if( ! check_tcs( conf, port, vfc->tcpctgs ) ) {
			snprintf( mbuf, sizeof( mbuf ), "TC percentages cause one or more total allocation to exceed 100%" );
			bleat_printf( 1, "vf not added: %s", mbuf );
			if( reason ) {
				*reason = strdup( mbuf );
			}
			return 0;
		}
	}

	if( vfc->start_cb != NULL && strchr( vfc->start_cb, ';' ) != NULL ) {
		snprintf( mbuf, sizeof( mbuf ), "start_cb command contains invalid character: ;" );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}
	if( vfc->stop_cb != NULL && strchr( vfc->stop_cb, ';' ) != NULL ) {
		snprintf( mbuf, sizeof( mbuf ), "stop_cb command contains invalid character: ;" );
		bleat_printf( 1, "vf not added: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

	// CAUTION: if we fail because of a parm error it MUST happen before here!
	if( vidx == port->num_vfs ) {		// inserting at end, bump the num we have used
		port->num_vfs++;
	}
	
	vf = &port->vfs[vidx];						// copy from config data doing any translation needed
	memset( vf, 0, sizeof( *vf ) );				// assume zeroing everything is good
	vf->owner = vfc->owner;
	vf->num = vfc->vfid;
	port->vfs[vidx].last_updated = ADDED;		// signal main code to configure the buggger
	vf->strip_stag = vfc->strip_stag;
	vf->insert_stag = vfc->strip_stag;			// both are pulled from same config parm
	vf->allow_bcast = vfc->allow_bcast;
	vf->allow_mcast = vfc->allow_mcast;
	vf->allow_un_ucast = vfc->allow_un_ucast;

	vf->allow_untagged = 0;					// for now these cannot be set by the config file data
	vf->vlan_anti_spoof = 1;
	vf->mac_anti_spoof = 1;

	vf->rate = 0.0;							// best effort :)
	vf->rate = vfc->rate;
	
	if( vfc->start_cb != NULL ) {
		vf->start_cb = strdup( vfc->start_cb );
	}
	if( vfc->stop_cb != NULL ) {
		vf->stop_cb = strdup( vfc->stop_cb );
	}

	vf->link = 0;							// default if parm missing or mis-set (not fatal)
	switch( *vfc->link_status ) {			// down, up or auto are allowed in config file
		case 'a':
		case 'A':
			vf->link = 0;					// auto is really: use what is configured in the PF	
			break;
		case 'd':
		case 'D':
			vf->link = -1;
			break;
		case 'u':
		case 'U':
			vf->link = 1;
			break;

		
		default:
			bleat_printf( 1, "link_status not recognised in config: %s; defaulting to auto", vfc->link_status );
			vf->link = 0;
			break;
	}
	
	for( i = 0; i < vfc->nvlans; i++ ) {
		vf->vlans[i] = vfc->vlans[i];
	}
	vf->num_vlans = vfc->nvlans;

	for( i = 0; i < vfc->nmacs; i++ ) {
		strcpy( vf->macs[i], vfc->macs[i] );		// we vet for length earlier, so this is safe.
	}
	vf->num_macs = vfc->nmacs;

	if( reason ) {
		*reason = NULL;
	}

	bleat_printf( 2, "VF was added: %s %s id=%d", vfc->name, vfc->pciid, vfc->vfid );
	free_config( vfc );
	return 1;
}

/*
	Get a list of all config files and add each one to the current config.
	If one fails, we will generate an error and ignore it.
*/
static void vfd_add_all_vfs(  parms_t* parms, sriov_conf_t* conf ) {
	char** flist; 					// list of files to pull in
	int		llen;					// list length
	int		i;

	if( parms == NULL || conf == NULL ) {
		bleat_printf( 0, "internal mishap: NULL conf or parms pointer passed to add_all_vfs" );
		return;
	}

	flist = list_files( parms->config_dir, "json", 1, &llen );
	if( flist == NULL || llen <= 0 ) {
		bleat_printf( 1, "zero vf configuration files (*.json) found in %s; nothing restored", parms->config_dir );
		return;
	}

	bleat_printf( 1, "adding %d existing vf configuration files to the mix", llen );

	
	for( i = 0; i < llen; i++ ) {
		bleat_printf( 2, "parsing %s", flist[i] );
		if( ! vfd_add_vf( conf, flist[i], NULL ) ) {
			bleat_printf( 0, "add_all_vfs: could not add %s", flist[i] );
		}
	}
	
	free_list( flist, llen );
}

/*
	Delete a VF from a port.  We expect the name of a file which we can read the
	parms from and suss out the pciid and the vfid.  Those are used to find the
	info in the global config and render it useless. The first thing we attempt
	to do is to remove or rename the config file.  If we can't do that we
	don't do anything else because we'd give the false sense that it was deleted
	but on restart we'd recreate it, or worse have a conflict with something that
	was added.
*/
static int vfd_del_vf( parms_t* parms, sriov_conf_t* conf, char* fname, char** reason ) {
	vf_config_t* vfc;					// raw vf config file contents	
	int	i;
	int vidx;							// index into the vf array
	struct sriov_port_s* port = NULL;	// reference to a single port in the config
	char mbuf[BUF_1K];					// message buffer if we fail
	
	if( conf == NULL || fname == NULL ) {
		bleat_printf( 0, "vfd_del_vf called with nil config or filename pointer" );
		if( reason ) {
			snprintf( mbuf, sizeof( mbuf), "internal mishap: config ptr was nil" );
			*reason = strdup( mbuf );
		}
		return 0;
	}

	if( (vfc = read_config( fname )) == NULL ) {
		snprintf( mbuf, sizeof( mbuf ), "unable to read config file: %s: %s", fname, errno > 0 ? strerror( errno ) : "unknown sub-reason" );
		bleat_printf( 1, "vfd_del_vf failed: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		return 0;
	}

	if( parms->delete_keep ) {											// need to keep the old by renaming it with a trailing -
		snprintf( mbuf, sizeof( mbuf ), "%s-", fname );
		if( rename( fname, mbuf ) < 0 ) {
			snprintf( mbuf, sizeof( mbuf ), "unable to rename config file: %s: %s", fname, strerror( errno ) );
			bleat_printf( 1, "vfd_del_vf failed: %s", mbuf );
			if( reason ) {
				*reason = strdup( mbuf );
			}
			free_config( vfc );
			return 0;
		}
	} else {
		if( unlink( fname ) < 0 ) {
			snprintf( mbuf, sizeof( mbuf ), "unable to delete config file: %s: %s", fname, strerror( errno ) );
			bleat_printf( 1, "vfd_del_vf failed: %s", mbuf );
			if( reason ) {
				*reason = strdup( mbuf );
			}
			free_config( vfc );
			return 0;
		}
	}

	bleat_printf( 2, "del: config data: name: %s", vfc->name );
	bleat_printf( 2, "del: config data: pciid: %s", vfc->pciid );
	bleat_printf( 2, "del: config data: vfid: %d", vfc->vfid );

	if( vfc->pciid == NULL || vfc->vfid < 1 ) {
		snprintf( mbuf, sizeof( mbuf ), "unable to read config file: %s", fname );
		bleat_printf( 1, "vfd_del_vf failed: %s", mbuf );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		free_config( vfc );
		return 0;
	}

	for( i = 0; i < conf->num_ports; i++ ) {						// find the port that this vf is attached to
		if( strcmp( conf->ports[i].pciid, vfc->pciid ) == 0 ) {	// match
			port = &conf->ports[i];
			break;
		}
	}

	if( port == NULL ) {
		snprintf( mbuf, sizeof( mbuf ), "%s: could not find port %s in the config", vfc->name, vfc->pciid );
		bleat_printf( 1, "vf not added: %s", mbuf );
		free_config( vfc );
		if( reason ) {
			*reason = strdup( mbuf );
		}
		return 0;
	}

	vidx = -1;
	for( i = 0; i < port->num_vfs; i++ ) {				// suss out the id that is listed
		if( port->vfs[i].num == vfc->vfid ) {			// this is it.
			vidx = i;
			break;
		}
	}

	if( vidx >= 0 ) {									//  it's there -- take down in the config
		port->vfs[vidx].last_updated = DELETED;			// signal main code to nuke the puppy (vfid stays set so we don't see it as a hole until it's gone)
	} else {
		bleat_printf( 1, "warning: del didn't find the pciid/vf combination in the active config: %s/%d", vfc->pciid, vfc->vfid );
	}
	
	if( reason ) {
		*reason = NULL;
	}
	bleat_printf( 2, "VF was deleted: %s %s id=%d", vfc->name, vfc->pciid, vfc->vfid );
	return 1;
}

// ---- request/response functions -----------------------------------------------------------------------------

/*
	Write to an open file des with a simple retry mechanism.  We cannot afford to block forever,
	so we'll try only a few times if we make absolutely no progress.
*/
static int vfd_write( int fd, const char* buf, int len ) {
	int	tries = 5;				// if we have this number of times where there is no progress we give up
	int	nsent;					// number of bytes actually sent
	int n2send;					// number of bytes left to send

	n2send = len;
	while( n2send > 0 && tries > 0 ) {
		nsent = write( fd, buf, n2send );			// hard error; quit immediately
		if( nsent < 0 ) {
			bleat_printf( 0, "WRN: write error attempting %d, wrote only %d bytes: %s", len, len - n2send, strerror( errno ) );
			return -1;
		}
			
		if( nsent == n2send ) {
			return len;
		}

		if( nsent > 0 ) { 		// something sent, so we assume iplex is actively reading
			n2send -= nsent;
			buf += nsent;
		} else {
			tries--;
			usleep(50000);			// .5s
		}
	}

	bleat_printf( 0, "WRN: write timed out attempting %d, but wrote only %d bytes", len, len - n2send );
	return -1;
}

/*
	Construct json to write onto the response pipe.  The response pipe is opened in non-block mode
	so that it will fail immiediately if there isn't a reader or the pipe doesn't exist. We assume
	that the requestor opens the pipe before sending the request so that if it is delayed after
	sending the request it does not prevent us from writing to the pipe.  If we don't open in 	
	blocked mode we could hang foever if the requestor dies/aborts.
*/
static void vfd_response( char* rpipe, int state, const char* msg ) {
	int 	fd;
	char	buf[BUF_1K];

	if( rpipe == NULL ) {
		return;
	}

	if( (fd = open( rpipe, O_WRONLY | O_NONBLOCK, 0 )) < 0 ) {
	 	bleat_printf( 0, "unable to deliver response: open failed: %s: %s", rpipe, strerror( errno ) );
		return;
	}

	if( bleat_will_it( 2 ) ) {
		bleat_printf( 2, "sending response: %s(%d) [%d] %d bytes", rpipe, fd, state, strlen( msg ) );
	} else {
		bleat_printf( 3, "sending response: %s(%d) [%d] %s", rpipe, fd, state, msg );
	}

	snprintf( buf, sizeof( buf ), "{ \"state\": \"%s\", \"msg\": \"", state ? "ERROR" : "OK" );
	if ( vfd_write( fd, buf, strlen( buf ) ) > 0 ) {
		if ( msg == NULL || vfd_write( fd, msg, strlen( msg ) ) > 0 ) {
			snprintf( buf, sizeof( buf ), "\" }\n" );				// terminate the json
			vfd_write( fd, buf, strlen( buf ) );
			bleat_printf( 2, "response written to pipe" );			// only if all of message written 
		}
	}

	bleat_pop_lvl();			// we assume it was pushed when the request received; we pop it once we respond
	close( fd );
}

/*
	Cleanup a request and free the memory.
*/
static void vfd_free_request( req_t* req ) {
	if( req->resource != NULL ) {
		free( req->resource );
	}
	if( req->resp_fifo != NULL ) {
		free( req->resp_fifo );
	}

	free( req );
}

/*
	Read an iplx request from the fifo, and format it into a request block.
	A pointer to the struct is returned; the caller must use vfd_free_request() to
	properly free it.
*/
static req_t* vfd_read_request( parms_t* parms ) {
	void*	jblob;				// json parsing stuff
	char*	rbuf;				// raw request buffer from the pipe
	char*	stuff;				// stuff teased out of the json blob
	req_t*	req = NULL;
	int		lvl;				// log level supplied

	rbuf = rfifo_read( parms->rfifo );
	if( ! *rbuf ) {				// empty, nothing to do
		free( rbuf );
		return NULL;
	}

	if( (jblob = jw_new( rbuf )) == NULL ) {
		bleat_printf( 0, "ERR: failed to create a json parsing object for: %s", rbuf );
		free( rbuf );
		return NULL;
	}

	if( (stuff = jw_string( jblob, "action" )) == NULL ) {
		bleat_printf( 0, "ERR: request received without action: %s", rbuf );
		free( rbuf );
		jw_nuke( jblob );
		return NULL;
	}

	
	if( (req = (req_t *) malloc( sizeof( *req ) )) == NULL ) {
		bleat_printf( 0, "ERR: memory allocation error tying to alloc request for: %s", rbuf );
		free( rbuf );
		jw_nuke( jblob );
		return NULL;
	}
	memset( req, 0, sizeof( *req ) );

	bleat_printf( 2, "raw message: (%s)", rbuf );

	switch( *stuff ) {				// we assume compiler builds a jump table which makes it faster than a bunch of nested string compares
		case 'a':
		case 'A':					// assume add until something else starts with a
			req->rtype = RT_ADD;
			break;

		case 'd':
		case 'D':
			if( strcmp( stuff, "dump" ) == 0 ) {
				req->rtype = RT_DUMP;
			} else {
				req->rtype = RT_DEL;
			}
			break;

		case 'p':					// ping
			req->rtype = RT_PING;
			break;

		case 's':
		case 'S':					// assume show
			req->rtype = RT_SHOW;
			break;

		case 'v':
			req->rtype = RT_VERBOSE;
			break;	

		default:
			bleat_printf( 0, "ERR: unrecognised action in request: %s", rbuf );
			jw_nuke( jblob );
			return NULL;
			break;
	}

	if( (stuff = jw_string( jblob, "params.filename")) != NULL ) {
		req->resource = strdup( stuff );
	} else {
		if( (stuff = jw_string( jblob, "params.resource")) != NULL ) {
			req->resource = strdup( stuff );
		}
	}
	if( (stuff = jw_string( jblob, "params.r_fifo")) != NULL ) {
		req->resp_fifo = strdup( stuff );
	}
	
	req->log_level = lvl = jw_missing( jblob, "params.loglevel" ) ? 0 : (int) jw_value( jblob, "params.loglevel" );
	bleat_push_glvl( lvl );					// push the level if greater, else push current so pop won't fail

	free( rbuf );
	jw_nuke( jblob );
	return req;
}

/*
	Request interface. Checks the request pipe and handles a reqest. If
	forever is set then this is a black hole (never returns).
	Returns true if it handled a request, false otherwise.
*/
static int vfd_req_if( parms_t *parms, sriov_conf_t* conf, int forever ) {
	req_t*	req;
	char	mbuf[2048];			// message and work buffer
	char*	buf;				// buffer gnerated by something else
	int		rc = 0;
	char*	reason;
	int		req_handled = 0;

	if( forever ) {
		bleat_printf( 1, "req_if: forever loop entered" );
	}

	*mbuf = 0;
	do {
		if( (req = vfd_read_request( parms )) != NULL ) {
			bleat_printf( 3, "got request" );
			req_handled = 1;

			switch( req->rtype ) {
				case RT_PING:
					snprintf( mbuf, sizeof( mbuf ), "pong: %s", version );
					vfd_response( req->resp_fifo, 0, mbuf );
					break;

				case RT_ADD:
					if( strchr( req->resource, '/' ) != NULL ) {									// assume fully qualified if it has a slant
						strcpy( mbuf, req->resource );
					} else {
						snprintf( mbuf, sizeof( mbuf ), "%s/%s", parms->config_dir, req->resource );
					}

					bleat_printf( 2, "adding vf from file: %s", mbuf );
					if( vfd_add_vf( conf, req->resource, &reason ) ) {		// read the config file and add to in mem config if ok
						if( vfd_update_nic( parms, conf ) == 0 ) {			// added to config was good, drive the nic update
							snprintf( mbuf, sizeof( mbuf ), "vf added successfully: %s", req->resource );
							vfd_response( req->resp_fifo, 0, mbuf );
							bleat_printf( 1, "vf added: %s", mbuf );
						} else {
							// TODO -- must turn the vf off so that another add can be sent without forcing a delete
							// 		update_nic always returns good now, so this waits until it catches errors and returns bad
							snprintf( mbuf, sizeof( mbuf ), "vf add failed: unable to configure the vf for: %s", req->resource );
							vfd_response( req->resp_fifo, 0, mbuf );
							bleat_printf( 1, "vf add failed nic update error" );
						}
					} else {
						snprintf( mbuf, sizeof( mbuf ), "unable to add vf: %s: %s", req->resource, reason );
						vfd_response( req->resp_fifo, 1, mbuf );
						free( reason );
					}
					if( bleat_will_it( 3 ) ) {					// TODO:  remove after testing
  						dump_sriov_config( conf );
					}
					break;

				case RT_DEL:
					if( strchr( req->resource, '/' ) != NULL ) {									// assume fully qualified if it has a slant
						strcpy( mbuf, req->resource );
					} else {
						snprintf( mbuf, sizeof( mbuf ), "%s/%s", parms->config_dir, req->resource );
					}

					bleat_printf( 1, "deleting vf from file: %s", mbuf );
					if( vfd_del_vf( parms, conf, req->resource, &reason ) ) {		// successfully updated internal struct
						if( vfd_update_nic( parms, conf ) == 0 ) {			// nic update was good too
							snprintf( mbuf, sizeof( mbuf ), "vf deleted successfully: %s", req->resource );
							vfd_response( req->resp_fifo, 0, mbuf );
							bleat_printf( 1, "vf deleted: %s", mbuf );
						} // TODO need else -- see above
					} else {
						snprintf( mbuf, sizeof( mbuf ), "unable to delete vf: %s: %s", req->resource, reason );
						vfd_response( req->resp_fifo, 1, mbuf );
						free( reason );
					}
					if( bleat_will_it( 3 ) ) {					// TODO:  remove after testing
  						dump_sriov_config( conf );
					}
					break;

				case RT_DUMP:									// spew everything to the log
					dump_dev_info( conf->num_ports);			// general info about each port
  					dump_sriov_config( conf );					// pf/vf specific info
					vfd_response( req->resp_fifo, 0, "dump captured in the log" );
					break;

				case RT_SHOW:
					if( parms->forreal ) {
						if( req->resource != NULL && strcmp( req->resource, "pfs" ) == 0 ) {				// dump just the VF information
							if( (buf = gen_stats( conf, 1 )) != NULL )  {		// todo need to replace 1 with actual number of ports
								vfd_response( req->resp_fifo, 0, buf );
								free( buf );
							} else {
								vfd_response( req->resp_fifo, 1, "unable to generate pf stats" );
							}
						} else {
							if( req->resource != NULL  &&  isdigit( *req->resource ) ) {						// dump just for the indicated pf (future)
								vfd_response( req->resp_fifo, 1, "show of specific PF is not supported in this release; use 'all' or 'pfs'." );
							} else {												// assume we dump for all
								if( (buf = gen_stats( conf, 0 )) != NULL )  {		// todo need to replace 1 with actual number of ports
									vfd_response( req->resp_fifo, 0, buf );
									free( buf );
								} else {
									vfd_response( req->resp_fifo, 1, "unable to generate stats" );
								}
							}
						}
					} else {
							vfd_response( req->resp_fifo, 1, "VFD running in 'no harm' (-n) mode; no stats available." );
					}
					break;

				case RT_VERBOSE:
					if( req->log_level >= 0 ) {
						bleat_set_lvl( req->log_level );
						bleat_push_lvl( req->log_level );			// save it so when we pop later it doesn't revert

						bleat_printf( 0, "verbose level changed to %d", req->log_level );
						snprintf( mbuf, sizeof( mbuf ), "verbose level changed to: %d", req->log_level );
					} else {
						rc = 1;
						snprintf( mbuf, sizeof( mbuf ), "loglevel out of range: %d", req->log_level );
					}

					vfd_response( req->resp_fifo, rc, mbuf );
					break;
					

				default:
					vfd_response( req->resp_fifo, 1, "dummy request handler: urrecognised request." );
					break;
			}

			vfd_free_request( req );
		}
		
		if( forever )
			sleep( 1 );
	} while( forever );

	return req_handled;			// true if we did something -- more frequent recall if we did
}
