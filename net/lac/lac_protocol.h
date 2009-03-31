#ifndef _LAC_PROTOCOL_H
#define _LAC_PROTOCOL_H
typedef enum 
{ 
   False = 0, 
   True = 1 
} Boolean;

enum 
{ 
   Off = False, 
   On = True 
};

typedef unsigned char   La_Octet;
typedef unsigned char   La_Byte;

typedef unsigned short  La_Short;
typedef unsigned short  Port_no;

typedef unsigned int   La_Unsigned;

typedef unsigned char   Mac_address[6];
typedef unsigned short  Ethertype;

/**
 * LA Timer defines
 */
typedef int Time;
typedef unsigned short Ticks;

typedef struct la_timer La_Timer;

struct la_timer
{
	La_Timer *next_timer;
	Boolean  active;		               /* timer state */
	Time     exp_ticks;                 /* max ticks */
	Time     ticks;                     /* current ticks */
	void (*expiry_fn)(void *);
	void  *expiry_arg;
};

typedef enum 
{
   Short_timeout = True, 
   Long_timeout  = False
} Lacp_timeout;

/**
 * LA : Default values
 */

/*
 * All timers have tolerance of +-250ms. (43.4.4)
 * Lac_ticks is the number of internal timer ticks per LAC tick.
 * The finer internal granularity allows extension of the model to
 * random or uncertain transmission delays.
 */
enum 
{ 
   Lac_ticks = 1 /* should be 1 not 10 */ 
};

/*
 * This example implementation takes a simple approach to transmit
 * scheduling. The tx_scheduler timer checks ntt frequently, and
 * transmits as needed subject to the maximum transmission rate
 * limitations imposed by the tx_engine.
 */
enum { Lac_tx_scheduling_ticks = 1 };

/*
 * All the following LAC timers operate in terms of LAC ticks. If
 * a dedicated timer approach was to be used for LAC timers, rather
 * than a period timer tick per port with the LAC component counting
 * down individual timers then the following would need to be multiplied
 * by Lac_ticks (above). 
 * 43.4.4.
 */
enum {
   Slow_Periodic_Time   =  30,
   Fast_Periodic_Time   =   1,
   Long_Timeout_Time    =  90,
   Short_Timeout_Time   =   3,
   Churn_Detection_Time =  60,
   Aggregate_Wait_Time  =   2,
   Tx_interval_ticks     =   1
};

/*
 * Default transmit interval.
 */
enum { Tx_interval_time     =   1 };
enum { Max_tx_per_interval  =   3 };

/*
 * Default settings for system characteristics.
 */
enum {
   Default_system_priority = 32768,
   Default_port_priority   = 32768,
   Default_key             = 1,
   Default_lacp_activity   = True,
   Default_lacp_timeout    = Long_timeout,
   Default_aggregation     = True,
   Default_collector_delay_time = 1
};

#define LACPDU_LEN sizeof(Lac_pdu)
#define ETH_ALEN			6				/* no. of octets in ethernet address */
#define LACPDU_ETH_HLEN		14				/* total octets in header*/	

extern const unsigned char Slow_Protocols_Multicast[ETH_ALEN];
enum { Slow_Protocols_Ethertype   = (Ethertype)0x8809};
enum {LACP_Subtype                = (La_Octet)1}; 
enum {LACP_Version                = (La_Octet)1};

enum {Marker_Subtype              = (La_Octet)2};
enum {Marker_Version              = (La_Octet)1}; 
enum {Marker_Information          = (La_Octet)1}; 
enum {Marker_Response_Information = (La_Octet)2}; 

typedef unsigned short     System_priority;
typedef unsigned char      System_id[6];

enum    {Null_system = (int)0};
enum    {Null_port   = (Port_no)0};

typedef La_Short  La_Key;
enum    {Null_key = (La_Key)0};

enum Lacp_enable {
   Lacp_enabled = True, 
   Lacp_disabled= False
};

/**
 * LAC : Key defines
 *
 */
#define LACP_KEY_LOW        1
#define LACP_KEY_HIGH    4095

#define LACP_KEY_DEFAULT LACP_KEY_LOW

#define LA_KEY_10MB     0x1000
#define LA_KEY_100MB    0x2000
#define LA_KEY_1000MB   0x4000
#define LA_KEY_10000MB  0x8000
#define LA_KEY_UNAGGRE  0x8000


/**
 * LA : LACPDU structure(43.4.2.2)
 */
typedef struct lacp_du
{
	//La_Byte da[6];                         /* 0  */
	//La_Byte sa[6];                         /* 6  */
	//La_Short len_type;                     /* 12 */
	La_Byte subtype;                       /* 14 */
	La_Byte version_number;                /* 15 */
	La_Byte first_tlv_type;                /* 16 */
	La_Byte actor_info_len;                /* 17 */
	System_priority actor_sys_pri;         /* 18 */
	System_id actor_sys;                   /* 20 */
	La_Short actor_key;                    /* 26 */
	La_Short actor_port_pri;               /* 28 */
	La_Short actor_port;                   /* 30 */
	La_Byte actor_state;                   /* 32 */
	La_Byte pad1[3];                       /* 33 */
	La_Byte second_tlv_type;               /* 36 */
	La_Byte partner_info_len;              /* 37 */
	System_priority partner_sys_pri;       /* 38 */
	System_id partner_sys;                 /* 40 */
	La_Short partner_key;                  /* 46 */
	La_Short partner_port_pri;             /* 48 */
	La_Short partner_port;                 /* 50 */
	La_Byte partner_state;                 /* 52 */
	La_Byte pad2[3];                       /* 53 */
	La_Byte third_tlv_type;                /* 56 */
	La_Byte collector_info_len;            /* 57 */
	La_Short collector_max_del;            /* 58 */
	La_Byte pad3[12];                      /* 60 */
	La_Byte fourth_tlv_type;               /* 72 */
	La_Byte terminator_len;                /* 73 */
	La_Byte pad4[50];					   /* 74 */
}Lac_pdu;		

#define Lacpdu_da_offset                 0
#define Lacpdu_sa_offset                 6
#define Lacpdu_etype_offset             12
#define Lacpdu_subtype_offset           14
#define Lacpdu_version_number_offset    15
#define Lacpdu_first_tlv_type_offset    16
#define Lacpdu_actor_info_len           17
#define Lacpdu_actor_sys_pri_offset     18
#define Lacpdu_actor_sys_id_offset      20
#define Lacpdu_actor_key_offset         26
#define Lacpdu_actor_port_pri_offset    28
#define Lacpdu_actor_port_offset        30
#define Lacpdu_actor_state_offset       32
#define Lacpdu_actor_padd1_offset       33
#define Lacpdu_actor_padd2_offset       34
#define Lacpdu_actor_padd3_offset       35
#define Lacpdu_second_tlv_type_offet    36
#define Lacpdu_partner_info_len_offet   37
#define Lacpdu_partner_sys_pri_offset   38
#define Lacpdu_partner_sys_id_offset    40
#define Lacpdu_partner_key_offset       46
#define Lacpdu_partner_port_pri_offset  48
#define Lacpdu_partner_port_offset      50
#define Lacpdu_partner_state_offset     52
#define Lacpdu_partner_padd1_offset     53
#define Lacpdu_partner_padd2_offset     54
#define Lacpdu_partner_padd3_offset     55
#define Lacpdu_third_tlv_type_offset    56
#define Lacpdu_collector_info_len_offset 57
#define Lacpdu_collector_max_del_offset  58
#define Lacpdu_fourth_tlv_type_offset    72
#define Lacpdu_terminator_len_offset     73

/**
 * Protocol State
 */

#define LA_ACTIVITY_STATE_BIT   0x00      /* Activity bit:   1=Active        */
#define LA_TIMEOUT_STATE_BIT    0x01      /* Timeout:        1=Short timeout */
#define LA_AGGREGABLE_STATE_BIT 0x02      /* Aggregable:     1=enable        */
#define LA_SYNC_STATE_BIT       0x03      /* Sync:           1=enable        */
#define LA_COLL_STATE_BIT       0x04      /* Collecting:     1=enable        */
#define LA_DIST_STATE_BIT       0x05      /* Distributing:   1=enable        */
#define LA_DEFAULTED_STATE_BIT  0x06      /* Defaulted:      1=enable        */
#define LA_EXPIRED_STATE_BIT    0x07      /* Expired:        1=enable        */

/* actor state */
#define ACT_ADM_GET_BIT(port, bit) ((port->actor_admin.state.state & (1 << (bit))) >> (bit))
#define ACT_ADM_SET_BIT(port, bit) (port->actor_admin.state.state |= 1 << (bit))
#define ACT_ADM_CLR_BIT(port, bit) (port->actor_admin.state.state &= ~(1 << (bit)))

#define ACT_OPER_GET_BIT(port, bit) ((port->actor.state.state & (1 << (bit))) >> (bit))
#define ACT_OPER_SET_BIT(port, bit) (port->actor.state.state |= 1 << (bit))
#define ACT_OPER_CLR_BIT(port, bit) (port->actor.state.state &= ~(1 << (bit)))

/* partner state */
#define PART_OPER_GET_BIT(port, bit) ((port->partner.state.state & (1 << (bit))) >> (bit))
#define PART_OPER_SET_BIT(port, bit) (port->partner.state.state |= 1 << (bit))
#define PART_OPER_CLR_BIT(port, bit) (port->partner.state.state &= ~(1 << (bit)))

/* struct Lac_info state */
#define LA_INFO_GET_BIT(info, bit) ((info->state.state & (1 << (bit))) >> (bit))
#define LA_INFO_SET_BIT(info, bit) (info->state.state |= 1 << (bit))
#define LA_INFO_CLR_BIT(info, bit) (info->state.state &= ~(1 << (bit)))

typedef struct
{
	unsigned lacp_activity   : 1;
	unsigned lacp_timeout    : 1;
	unsigned aggregation     : 1;
	unsigned synchronization : 1;
	unsigned collecting      : 1;
	unsigned distributing    : 1;
	unsigned defaulted       : 1;
	unsigned expired         : 1;
} Lac_state_mask;

typedef union
{
   	Lac_state_mask bmask;
   	unsigned char state;
}Lac_state;

typedef struct lac_info
{
   System_priority system_priority;
   System_id       system_id;
   La_Key          key;
   La_Short        port_priority;
   Port_no         port_no;
   Lac_state       state;
} Lac_info;

typedef struct lac_packet/* Lac_packet */
{
   //Mac_address slow_protocols_address;
   //Ethertype   ethertype;
   //La_Octet    protocol_subtype;
   //La_Octet    protocol_version;   
   Lac_info    actor;
   Lac_info    partner;
   //La_Byte     pad[50];
} Lac_packet;

/**
 * LAC Port interface.
 */

enum interface_state
{
   Interface_link_off=0,
   Interface_link_on 
};

enum interface_dup
{
   Interface_half_dup=0, 
   Interface_full_dup
};

enum interface_speed
{
   Interface_10Mb = 0, 
   Interface_100Mb, 
   Interface_1000Mb, 
   Interface_10000Mb
};

/**
 * State machine event
 */

typedef enum 
{
	Lac_init,
	Lac_tick,
	Lac_port_enabled,
	Lac_port_disabled,
	Lac_tx_ntt,
	Lac_txd,
	Lac_received,
	Lac_new_info,
	Lac_check_moved
} Lac_event;

/**
 * State Machine Data
 */

/* Rx state */
enum rxm_state
{ 
   Rxm_initialize, 
   Rxm_port_disabled, 
   Rxm_lacp_disabled, 
   Rxm_expired,    
   Rxm_defaulted,     
   Rxm_current
};

/* Port_select */
enum port_select
{ 
   Unselected = 0, 
   Selected, 
   Standby
};

/* Mux Machine state */
enum mux_state
{ 
   Mux_Detached = 0, 
   Mux_Waiting, 
   Mux_Attached, 
   Mux_Collecting,
   Mux_Distributing
};

/**
 * Port statistic data structure
 * TODO: use statistics
 */

typedef struct lac_port_stats
{
   unsigned int lacpPDuRx;
   unsigned int lacpPDuTx;
   unsigned int markerPduRx;
   unsigned int markerPduTx;
   unsigned int markerRespPduRx;
   unsigned int markerRespPduTx;
   unsigned int OamPduRx; 
   unsigned int OamPduTx; 
   unsigned int unknownPduRx; 
   unsigned int illegalPduRx;
   unsigned int lacpPortDown;
}Lac_port_stats;

/**
 *  LAG ID: unused
 */
typedef struct lag_id Lag_id;

struct lag_id
{
   La_Short sys_pri;
   System_id sys_id;
   La_Short key;
};

/**
 * LAG port configuration
 */

typedef struct lac_port_cfg_mask
{
   unsigned port_enabled : 1;
   unsigned lacp_enabled : 1;
   unsigned port_moved   : 1;
   unsigned ready_N      : 1;
   unsigned ntt          : 1;
   unsigned actor_churn  : 1;
   unsigned partner_churn: 1;
   unsigned partition    : 1;
   unsigned port_ready   : 1;
   unsigned oper_coll_dis: 1;
   unsigned dplex        : 1;
   unsigned pstat        : 1;
   unsigned speed        : 4;
   unsigned poll_count   : 4;
   unsigned dplex_count  : 4;
   unsigned pstat_count  : 4;
   unsigned speed_count  : 4;
} Lac_port_cfg_mask;

/**
 * LA : System data structure
 */
typedef struct lac_system Lac_system;

struct lac_system
{
	System_priority sPriority;                /* System priority    */
	System_id       sysId;                    /* System ID          */
	La_Timer        tick_timer;               /* System tick timer  */
	int             collector_delay_time;     /* Delay time in tens of micro seconds */
};


typedef struct dist_hash_table Lac_hash_table;

struct dist_hash_table 
{
	struct dist_hash_table *next;
	unsigned int id; /* packet id */
	unsigned long saddr; /* source IPv4 address */
	unsigned long daddr; /* destination IPv4 address */
	int hash;
};


#endif
