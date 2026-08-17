package com.jitong.im.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ChatScreen(vm: MainViewModel) {
    val peer by vm.chatPeer.collectAsStateWithLifecycle()
    val allMessages by vm.messages.collectAsStateWithLifecycle()
    val p = peer ?: return
    val conv = allMessages[p.id].orEmpty()

    val listState = rememberLazyListState()
    LaunchedEffect(conv.size) {
        if (conv.isNotEmpty()) listState.animateScrollToItem(conv.size - 1)
    }

    var input by rememberSaveable { mutableStateOf("") }

    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(p.nick)
                        Text(
                            if (p.online) "在线" else "离线",
                            fontSize = 12.sp,
                            color = if (p.online) Color(0xFF07C160) else Color.Gray,
                        )
                    }
                },
                navigationIcon = {
                    TextButton(onClick = { vm.backToFriends() }) { Text("返回") }
                },
            )
        },
    ) { padding ->
        Column(
            Modifier
                .fillMaxSize()
                .padding(padding)
                .imePadding(),
        ) {
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .weight(1f)
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                items(conv, key = { it.msgId }) { msg ->
                    MessageBubble(msg)
                }
            }

            Row(
                Modifier
                    .fillMaxWidth()
                    .padding(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                OutlinedTextField(
                    value = input,
                    onValueChange = { input = it },
                    modifier = Modifier.weight(1f),
                    placeholder = { Text("输入消息…") },
                    maxLines = 3,
                )
                Button(
                    onClick = {
                        vm.send(input)
                        input = ""
                    },
                    modifier = Modifier.padding(start = 8.dp),
                ) { Text("发送") }
            }
        }
    }
}

@Composable
private fun MessageBubble(msg: ChatMessage) {
    Column(
        Modifier.fillMaxWidth(),
        horizontalAlignment = if (msg.fromMe) Alignment.End else Alignment.Start,
    ) {
        Box(
            Modifier
                .widthIn(max = 280.dp)
                .background(
                    if (msg.fromMe) Color(0xFF95EC69) else Color.White,
                    RoundedCornerShape(8.dp),
                )
                .padding(horizontal = 12.dp, vertical = 8.dp),
        ) {
            Text(msg.text)
        }
        if (msg.fromMe) {
            Text(
                when (msg.status) {
                    ChatMessage.Status.SENDING -> "发送中…"
                    ChatMessage.Status.DELIVERED -> "已送达"
                    ChatMessage.Status.OFFLINE_STORED -> "对方离线，已转存"
                    ChatMessage.Status.RECEIVED -> ""
                },
                fontSize = 10.sp,
                color = Color.Gray,
            )
        }
    }
}
